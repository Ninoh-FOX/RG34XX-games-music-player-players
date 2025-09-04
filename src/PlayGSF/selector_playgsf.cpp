#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string>
#include <vector>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <pthread.h>
#include <fcntl.h>

extern "C" {
#include "VBA/psftag.h"
}

#define SCREEN_WIDTH 720
#define SCREEN_HEIGHT 480
#define FONT_SIZE 24
#define MUSIC_ROOT "/mnt/mmc/music/GBA"
#define DISP_LCD_SET_BRIGHTNESS 0x102
#define DISP_LCD_GET_BRIGHTNESS 0x103

struct Entry {
    std::string name;
    bool is_dir;
};

enum Mode { MODE_LIST, MODE_PLAYBACK };
enum LoopMode { LOOP_OFF, LOOP_ONE, LOOP_ALL };

Mode mode = MODE_LIST;
LoopMode loop_mode = LOOP_ALL;
std::vector<Entry> entries;
std::string current_path = MUSIC_ROOT;
int selected_index = 0;
int scroll_offset = 0;
int scroll_offset_title = 0;
static Uint32 scroll_start_time_game = 0;
static Uint32 scroll_start_time_title = 0;
static Uint32 scroll_start_time_artist = 0;

const int scroll_speed = 20;
const int scroll_delay = 4000;

bool bass_enabled_local = false;

pid_t playgsf_pid = -1;
bool paused = false;
bool screen_off = false;

SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;
TTF_Font* font = nullptr;

const int TRIGGER_THRESHOLD = 16000;
bool l2_prev = false, r2_prev = false;

// NUEVAS BANDERAS DE CONTROL SALTO MANUAL
bool manual_switch = false;
bool manual_forward = true;

// Estructura de metadatos de pista
struct TrackMetadata {
    std::string filename, title, artist, game, year, copyright, gsf_by, length, fade;
};

// Clamp manual (C++14)
static void clamp_index(int& idx, int low, int high) {
    if (idx < low) idx = low;
    if (idx > high) idx = high;
}

// ----- MONITORING BATTERY -----
static int battery = 0; // battery percentage from /sys/class/power_supply/battery/capacity
static Uint32 last_battery_update = 0;
static const Uint32 battery_update_interval = 1000; // 1 second in ms

static int last_brightness = 50;

std::string state_file_path() {
    std::string dir = "/.config/playgsf";
    mkdir(dir.c_str(), 0755);
    return dir + "/state.txt";
}

void hw_display_off(void);
void hw_display_on(void);

int get_brightness() {
    int val = -1;
    int fd = open("/dev/disp", O_RDWR);
    if (fd >= 0) {
        unsigned long param[4] = {0UL, 0UL, 0UL, 0UL};
        if (ioctl(fd, DISP_LCD_GET_BRIGHTNESS, param) != -1) {
            val = (int)param[1];
        }
        close(fd);
    }

    if (val <= 0) {
        FILE* fp = fopen("/.config/.keymon_brightness", "r");
        if (fp) {
            int level = 3;
            int brightness_map[8] = {5, 10, 20, 50, 70, 140, 200, 255};
            if (fscanf(fp, "%d", &level) == 1) {
                if (level < 0) level = 0;
                if (level > 7) level = 7;
                val = brightness_map[level];
            }
            fclose(fp);
        } else {
            val = 50;
        }
    }
    return val;
}


void set_brightness(int val) {
    int fd = open("/dev/disp", O_RDWR);
    if (fd >= 0) {
        unsigned long param[4] = {0, (unsigned long)val, 0, 0};
        ioctl(fd, DISP_LCD_SET_BRIGHTNESS, &param);
        close(fd);
    }
}

void set_fb_blank(int val) {
    FILE *f = fopen("/sys/class/graphics/fb0/blank", "w");
    if (f) {
        fprintf(f, "%d\n", val);
        fclose(f);
    }
}

// Hardware functions to turn display on/off
void hw_display_off(void)
{
	last_brightness = get_brightness();
    set_fb_blank(4);
	set_brightness(0);
}

void hw_display_on(void)
{
	set_fb_blank(0);     
    set_brightness(last_brightness);
}

int read_battery_percent() {
    FILE* f = fopen("/sys/class/power_supply/axp2202-battery/capacity", "r");

    if (!f) return -1;

    int percent = -1;
    if (fscanf(f, "%d", &percent) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);

    if (percent < 0) percent = 0;
    else if (percent > 100) percent = 100;
    return percent;
}

bool is_directory(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
}

bool is_valid_music(const std::string& fname) {
    auto pos = fname.find_last_of('.');
    if (pos == std::string::npos) return false;
    std::string ext = fname.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return (ext == ".minigsf" || ext == ".gsf");
}

void list_directory(const std::string& path, bool reset_selection = true) {
    entries.clear();
    DIR* dir = opendir(path.c_str());
    if (!dir) return;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        std::string full_path = path + "/" + name;
        bool dir_flag = is_directory(full_path);
        if (dir_flag || is_valid_music(name)) {
            entries.emplace_back(Entry{name, dir_flag});
        }
    }
    closedir(dir);
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b){
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return a.name < b.name;
    });
    if (reset_selection) {
        selected_index = 0;
        scroll_offset = 0;
    }
    clamp_index(selected_index, 0, (int)entries.size() - 1);
}

void kill_playgsf() {
    if (playgsf_pid > 0) {
        kill(playgsf_pid, SIGTERM);
        paused = false;
    }
}

bool launch_playgsf(const std::string& filepath) {
    if (playgsf_pid != -1) return false;
    pid_t pid = fork();
    if (pid == 0) {
        
        if (bass_enabled_local)
            execl("/usr/bin/playgsf", "playgsf", "-s", "-q", "-b", filepath.c_str(), nullptr);
        else
            execl("/usr/bin/playgsf", "playgsf", "-s", "-q", filepath.c_str(), nullptr);

        _exit(127);
    } else if (pid > 0) {
        playgsf_pid = pid;
        paused = false;
        return true;
    }
    return false;
}

int find_next_track(int current, bool forward = true) {
    int idx = current;
    int size = (int)entries.size();
    if (size == 0) return -1;
    do {
        idx = forward ? (idx + 1) % size : (idx - 1 + size) % size;
        if (!entries[idx].is_dir && is_valid_music(entries[idx].name))
            return idx;
    } while (idx != current);
    return current;
}

// Parse de etiquetas length en formato "m:ss", "ss" o "ss.xxx"
int parse_length(const std::string& str) {
    if (str.empty()) return 0;
    int min = 0, sec = 0;
    size_t colon = str.find(':');
    size_t dot = str.find('.');
    try {
        if (colon != std::string::npos) {
            min = std::stoi(str.substr(0, colon));
            sec = std::stoi(str.substr(colon + 1));
            return min * 60 + sec;
        } else if (dot != std::string::npos) {
            return std::stoi(str.substr(0, dot));
        }
        return std::stoi(str);
    } catch(...) {
        return 0;
    }
}

bool read_metadata(const std::string& file, TrackMetadata& out) {
    out = TrackMetadata{};
    char tag[50001] = {0};
    if (psftag_readfromfile((void*)tag, file.c_str())) return false;

    char buf[512] = {0};
    out.filename = file;
    if (psftag_getvar(tag, "title", buf, sizeof(buf)) == 0) out.title = buf;
    if (psftag_getvar(tag, "artist", buf, sizeof(buf)) == 0) out.artist = buf;
    if (psftag_getvar(tag, "game", buf, sizeof(buf)) == 0) out.game = buf;
    if (psftag_getvar(tag, "year", buf, sizeof(buf)) == 0) out.year = buf;
    if (psftag_getvar(tag, "copyright", buf, sizeof(buf)) == 0) out.copyright = buf;
    if (psftag_getvar(tag, "gsfby", buf, sizeof(buf)) == 0) out.gsf_by = buf;

	if (psftag_getvar(tag, "length", buf, sizeof(buf)) == 0) {
        out.length = buf;
    } else {
        out.length = "150";
    }

    if (psftag_getvar(tag, "fade", buf, sizeof(buf)) == 0) {
        out.fade = buf;
    } else {
        out.fade = "10";
    }

    return true;
}

int total_track_seconds(const TrackMetadata& m) {
    int len = parse_length(m.length);
    int fd = parse_length(m.fade);
    return len + fd;
}

void render_text(const std::string& text, int x, int y, SDL_Color color) {
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    if (!tex) return;
    SDL_Rect dst = {x, y, 0, 0};
    SDL_QueryTexture(tex, nullptr, nullptr, &dst.w, &dst.h);
    SDL_RenderCopy(renderer, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
}

void render_scrolling_text(const std::string &text, int x, int y, int max_width, SDL_Color color, Uint32& scroll_start_time) {
    int text_width = 0, text_height = 0;
    TTF_SizeText(font, text.c_str(), &text_width, &text_height);

    SDL_Rect clip_rect = {x, y, max_width, text_height};
    SDL_RenderSetClipRect(renderer, &clip_rect);

    Uint32 now = SDL_GetTicks();

    if (text_width > max_width) {
        if (scroll_start_time == 0) scroll_start_time = now;
        Uint32 elapsed = now - scroll_start_time;

        if (elapsed > (Uint32)scroll_delay) {
            int scroll_distance = text_width + max_width + SCREEN_WIDTH / 3;
            int scroll_pixels = (int)((elapsed - scroll_delay) * scroll_speed / 1000) % scroll_distance;

            int render_x = x - scroll_pixels;

            SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text.c_str(), color);
            SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);

            SDL_Rect dst = {render_x, y, text_width, text_height};
            SDL_RenderCopy(renderer, tex, nullptr, &dst);

            if (render_x + text_width < x + max_width) {
                dst.x = render_x + text_width + SCREEN_WIDTH / 3;
                SDL_RenderCopy(renderer, tex, nullptr, &dst);
            }

            SDL_DestroyTexture(tex);
            SDL_FreeSurface(surf);

            SDL_RenderSetClipRect(renderer, nullptr);
            return;
        }
    }

    SDL_RenderSetClipRect(renderer, nullptr);
    render_text(text, x, y, color);
}

void render_status_monitor(int screen_width) {
    SDL_Color green = {0, 255, 0, 255};
    SDL_Color orange = {255, 165, 0, 255};

    const char* bat_label = "BAT:";
    char bat_value[16];
    snprintf(bat_value, sizeof(bat_value), "%d%% ", battery);

    int bat_label_w = 0, bat_value_w = 0;
    int h = 0;

    TTF_SizeText(font, bat_label, &bat_label_w, &h);
    TTF_SizeText(font, bat_value, &bat_value_w, &h);

    int pad = 0;
    int total_w = bat_label_w + bat_value_w + pad * 3;

    int x = screen_width - 10 - total_w;  // usar parámetro
    int y = 10;

    render_text(bat_label, x, y, green);
    x += bat_label_w;
    render_text(bat_value, x, y, orange);
}

int parse_time_string(const std::string& time_str) {
    std::string s = time_str;
    size_t dot = s.find('.');
    if (dot != std::string::npos) {
        s = s.substr(0, dot);
    }
    size_t colon = s.find(':');
    int min = 0, sec = 0;
    try {
        if (colon != std::string::npos) {
            min = std::stoi(s.substr(0, colon));
            sec = std::stoi(s.substr(colon + 1));
        } else {
            sec = std::stoi(s);
        }
    } catch (...) {
        return 0;
    }
    return min * 60 + sec;
}

void render_black_screen() {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);
}

void draw_list() {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_Color white = {255,255,255,255};
    SDL_Color highlight = {255,255,0,255};
    SDL_Color dir_color = {0,255,255,255};
    int lh = TTF_FontLineSkip(font);
    int help_h = lh * 4;
    int max_lines = (SCREEN_HEIGHT - help_h) / lh;
    if (max_lines < 1) max_lines = 1;
    int total = (int)entries.size();
    clamp_index(selected_index, 0, total > 0 ? total - 1 : 0);
    if (total == 0) {
        render_text("No items found", 30, 50, white);
        SDL_RenderPresent(renderer);
        return;
    }
    if (selected_index <= max_lines / 2)
        scroll_offset = 0;
    else if (selected_index >= total - max_lines / 2)
        scroll_offset = std::max(0, total - max_lines);
    else
        scroll_offset = selected_index - max_lines / 2;
    render_text("Directory: " + current_path, 5, 2, white);
    int y = lh + 5;
    for (int i = scroll_offset; i < total && i < scroll_offset + max_lines; ++i) {
        SDL_Color color = (i == selected_index) ? highlight : (entries[i].is_dir ? dir_color : white);
        std::string prefix = entries[i].is_dir ? "[DIR] " : " ";
        render_text(prefix + entries[i].name, 10, y, color);
        y += lh;
    }
    int hy = SCREEN_HEIGHT - 60;
    render_text("A: Play/Enter  B: Back  L1/R1: Jump", 10, hy + lh*0, white);
    render_text("SL: Exit  Menu: Lock", 10, hy + lh*1, white);

    SDL_RenderPresent(renderer);
}

void draw_playback(const TrackMetadata& meta, int elapsed) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    SDL_Color green = {0, 255, 0, 255};
    SDL_Color orange = {255, 165, 0, 255};

    int max_label_width = 0;
    int h = 0;
    const char* labels[] = {"Game:", "Title:", "Artist:", "Length:", "Elapsed:", "Year:", "GSF By:", "Copyright:"};
    for (auto label : labels) {
        int w = 0;
        TTF_SizeText(font, label, &w, &h);
        if (w > max_label_width) max_label_width = w;
    }
    const int padding = 10;
    
    // Calcular progreso total (length + fade)
    int length_sec = parse_time_string(meta.length);
    int fade_sec = parse_time_string(meta.fade);
    if (fade_sec == 0) fade_sec = 10;

    int total_seconds;
        if (loop_mode == LOOP_ONE) {
            total_seconds = length_sec;
        } else {
            total_seconds = length_sec + fade_sec;
        }
    
    int x_text = 20 + max_label_width + padding;
    int max_width = SCREEN_WIDTH - x_text - 10;

    int y = 20;
    render_text("Now Playing...", 20, y, green);
    y += 40;
    
    if (!meta.game.empty()) {
        render_text("Game:", 20, y, green);
        render_scrolling_text(meta.game, x_text, y, max_width, orange, scroll_start_time_game);
        y += 30;
    }
    
    if (!meta.title.empty()) {
        render_text("Title:", 20, y, green);
        render_scrolling_text(meta.title, x_text, y, max_width, orange, scroll_start_time_title);
        y += 30;
    }
    
    if (!meta.artist.empty()) {
        render_text("Artist:", 20, y, green);
        render_scrolling_text(meta.artist, x_text, y, max_width, orange, scroll_start_time_artist);
        y += 30;
    }


    {
        int min = total_seconds / 60;
        int sec = total_seconds % 60;
        char formatted_length[16];
        snprintf(formatted_length, sizeof(formatted_length), "%02d:%02d", min, sec);

        render_text("Length:", 20, y, green);
        render_text(formatted_length, 20 + max_label_width + padding, y, orange);
        y += 30;
    }

    {
        int min = elapsed / 60;
        int sec = elapsed % 60;
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", min, sec);
        render_text("Elapsed:", 20, y, green);
        render_text(buf, 20 + max_label_width + padding, y, orange);
        y += 30;
    }
    if (!meta.year.empty()) {
        render_text("Year:", 20, y, green);
        render_text(meta.year, 20 + max_label_width + padding, y, orange);
        y += 30;
    }
    if (!meta.gsf_by.empty()) {
        render_text("GSF By:", 20, y, green);
        render_text(meta.gsf_by, 20 + max_label_width + padding, y, orange);
        y += 30;
    }
    if (!meta.copyright.empty()) {
        render_text("Copyright:", 20, y, green);
        render_text(meta.copyright, 20 + max_label_width + padding, y, orange);
        y += 30;
    }

    // Dibujar barra de progreso entre Copyright y Loop
    int y_progress = y + (SCREEN_HEIGHT - 100 - y) / 2;

    int bar_x = 20;
    int bar_w = SCREEN_WIDTH - 40;
    int bar_h = 20;

    SDL_Rect border_rect = {bar_x, y_progress, bar_w, bar_h};
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255); // borde blanco
    SDL_RenderDrawRect(renderer, &border_rect);

    float progress = 0.0f;
    if (total_seconds > 0) {
        progress = (float)elapsed / total_seconds;
        if (progress > 1.0f) progress = 1.0f;
    }

    SDL_Rect fill_rect = {bar_x + 1, y_progress + 1, (int)((bar_w - 2) * progress), bar_h - 2};
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255); // barra roja
    SDL_RenderFillRect(renderer, &fill_rect);

    // Color para bass (activo/inactivo)
    SDL_Color bass_color = bass_enabled_local ? SDL_Color{255, 0, 0, 255} : SDL_Color{120, 100, 0, 255};
    int base_x = 10;
    int y_pos = SCREEN_HEIGHT - 100;
    
    // Mostrar [BASS]
    render_text("[BASS]", base_x, y_pos, bass_color);
    
    int bass_w = 0, bass_h = 0;
    TTF_SizeText(font, "[BASS]", &bass_w, &bass_h);
    
    // Mostrar [PAUSED] o [PLAYING] justo a la derecha de [BASS]
    std::string paused_text = "[PAUSED]";
    std::string playing_text = "[PLAYING]";
    
    SDL_Color paused_color = paused ? SDL_Color{255, 165, 0, 255} : SDL_Color{120, 100, 0, 255};
    SDL_Color playing_color = !paused ? SDL_Color{255, 165, 0, 255} : SDL_Color{120, 100, 0, 255};
    
    int paused_x = base_x + bass_w + 30;
    render_text(paused_text, paused_x, y_pos, paused_color);
    
    int paused_w = 0, paused_h = 0;
    TTF_SizeText(font, paused_text.c_str(), &paused_w, &paused_h);
    
    int playing_x = paused_x + paused_w + 30;
    render_text(playing_text, playing_x, y_pos, playing_color);
    
    // Finalmente, mantener el renderizado de Loop: y su valor con el espacio que corresponda
    std::string looptxt = (loop_mode == LOOP_ALL) ? "ALL" : (loop_mode == LOOP_ONE) ? "ONE" : "OFF";
    
    int loop_label_x = SCREEN_WIDTH - 140; // O ajustar según espacio necesario
    int loop_value_x = loop_label_x + 70;

    render_text("Loop:", loop_label_x, y_pos, green);
    render_text(looptxt, loop_value_x, y_pos, orange);

    render_text("B:Back L2/R2:Prev/Next Y:Loop Mode X:Bass Mode", 10, SCREEN_HEIGHT - 70, green);
    render_text("ST:Pause  SL:exit  Menu:Lock", 10, SCREEN_HEIGHT - 40, green);

    render_status_monitor(SCREEN_WIDTH);
    SDL_RenderPresent(renderer);
}


int main() {
    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError()); return 1; }
    if (TTF_Init() != 0) { fprintf(stderr, "TTF_Init error: %s\n", TTF_GetError()); SDL_Quit(); return 1; }
    window = SDL_CreateWindow("playgsf selector", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS);
    if (!window) { fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError()); TTF_Quit(); SDL_Quit(); return 1; }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) { fprintf(stderr, "SDL_CreateRenderer error: %s\n", SDL_GetError()); SDL_DestroyWindow(window); TTF_Quit(); SDL_Quit(); return 1; }
    font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", FONT_SIZE);
    if (!font) { fprintf(stderr, "TTF_OpenFont error\n"); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); TTF_Quit(); SDL_Quit(); return 1; }
    SDL_Joystick* joystick = nullptr;
    SDL_GameController* controller = nullptr;
    
    if (SDL_NumJoysticks() > 0) {
        joystick = SDL_JoystickOpen(0);
        if (!joystick) fprintf(stderr, "Error al abrir joystick: %s\n", SDL_GetError());
    }
    
    if (SDL_NumJoysticks() > 0 && SDL_IsGameController(0)) {
        controller = SDL_GameControllerOpen(0);
        if (!controller) fprintf(stderr, "Error al abrir gamecontroller: %s\n", SDL_GetError());
    }

    TrackMetadata current_meta;
    int track_seconds = 0;
    using clock_type = std::chrono::steady_clock;
    auto playback_start = clock_type::now();
    clock_type::time_point paused_at;
    int paused_seconds_total = 0;
    int elapsed_seconds = 0;
	
	last_battery_update = SDL_GetTicks();

    list_directory(current_path, true);
    
    {
        std::ifstream ifs(state_file_path());
        if (ifs) {
            std::string last_path, last_name, last_type, last_bass;;
            std::getline(ifs, last_path);
            std::getline(ifs, last_name);
            std::getline(ifs, last_type);
            std::getline(ifs, last_bass);
            
            if (!last_bass.empty())
                bass_enabled_local = (last_bass == "1");
    
            if (!last_path.empty() && is_directory(last_path)) {
                current_path = last_path;
                list_directory(current_path, true);
    
                if (!last_name.empty()) {
                    for (size_t i = 0; i < entries.size(); i++) {
                        if (entries[i].name == last_name) {
                            selected_index = (int)i;
                            if (last_type == "DIR" && last_path != MUSIC_ROOT) {
                                current_path += "/" + last_name;
                                list_directory(current_path, true);
                            }
                            break;
                        }
                    }
                }
            }
        }
    }
    
    draw_list();
    bool running = true;
    SDL_Event e;

    while (running) {
        SDL_Delay(16);

		Uint32 now = SDL_GetTicks();
		if (now - last_battery_update >= battery_update_interval) {
		    int new_battery = read_battery_percent();
		    if (new_battery >= 0) battery = new_battery;
		    last_battery_update = now;
		}
        
        // ---- CONTROL DEL FIN DE PISTA y CAMBIO CENTRALIZADO ----
        if (playgsf_pid > 0 && !paused) {
            int status;
            pid_t ret = waitpid(playgsf_pid, &status, WNOHANG);
            if (ret == playgsf_pid) {
                playgsf_pid = -1;
                if (mode == MODE_PLAYBACK) {
                    if (manual_switch) {
                        int next_track = find_next_track(selected_index, manual_forward);
                        if (next_track != selected_index) selected_index = next_track;
                        manual_switch = false;
                        std::string filepath = current_path + "/" + entries[selected_index].name;
                        if (read_metadata(filepath, current_meta)) {
                            if (loop_mode == LOOP_ONE) {
                                track_seconds = parse_length(current_meta.length);
                            } else {
                                track_seconds = total_track_seconds(current_meta);
                            }
                            playback_start = clock_type::now();
                            paused_seconds_total = 0;
                            scroll_start_time_game = 0;
                            scroll_start_time_title = 0;
                            scroll_start_time_artist = 0;
                        }
                        launch_playgsf(filepath);
						if (!screen_off) {
                        draw_playback(current_meta, 0);
						}
                        mode = MODE_PLAYBACK; paused = false;
                    } else {
                        // FIN DE PISTA AUTOMÁTICO
                        if (track_seconds > 0) {
                            if (loop_mode == LOOP_OFF) {
                                mode = MODE_LIST;
                                draw_list();
                            } else if (loop_mode == LOOP_ONE) {
                                std::string filepath = current_path + "/" + entries[selected_index].name;
                                if (read_metadata(filepath, current_meta)) {
                                    track_seconds = parse_length(current_meta.length);
                                    playback_start = clock_type::now();
                                    paused_seconds_total = 0;
                                    scroll_start_time_game = 0;
                                    scroll_start_time_title = 0;
                                    scroll_start_time_artist = 0;
                                }
                                launch_playgsf(filepath);
                                if (!screen_off) {
                                draw_playback(current_meta, 0);
                                }
                                mode = MODE_PLAYBACK; paused = false;
                            } else if (loop_mode == LOOP_ALL) {
                                int next_track = find_next_track(selected_index, true);
                                if (next_track != selected_index) selected_index = next_track;
                                std::string filepath = current_path + "/" + entries[selected_index].name;
                                if (read_metadata(filepath, current_meta)) {
                                    track_seconds = total_track_seconds(current_meta);
                                    playback_start = clock_type::now();
                                    paused_seconds_total = 0;
                                    scroll_start_time_game = 0;
                                    scroll_start_time_title = 0;
                                    scroll_start_time_artist = 0;
                                }
                                launch_playgsf(filepath);
                                if (!screen_off) {
                                draw_playback(current_meta, 0);
                                }
                                mode = MODE_PLAYBACK; paused = false;
                            }
                        }
                    }
                }
            }
        }

        // ------ CONTROL DE TIEMPO: MATAR PROCESO si termina -----
        if (mode == MODE_PLAYBACK && playgsf_pid > 0 && !paused) {
            auto now = clock_type::now();
            elapsed_seconds = (int)std::chrono::duration_cast<std::chrono::seconds>(now - playback_start).count() - paused_seconds_total;
            int fade_sec = parse_length(current_meta.fade);
            if (fade_sec == 0) fade_sec = 10;
            if (loop_mode == LOOP_OFF) {
            if (track_seconds > 0 && elapsed_seconds >= track_seconds + 1) {
                manual_switch = false; // Fin natural
                kill_playgsf(); // Solo matar proceso, waitpid central decide siguiente acción
                }
            } else if (loop_mode == LOOP_ONE) {
            if (track_seconds > 0 && elapsed_seconds >= track_seconds + 1) {
                manual_switch = false; // Fin natural
                kill_playgsf(); // Solo matar proceso, waitpid central decide siguiente acción
                }
            } else {
            if (track_seconds > 0 && elapsed_seconds >= track_seconds + fade_sec) {
                manual_switch = false; // Fin natural
                kill_playgsf(); // Solo matar proceso, waitpid central decide siguiente acción
                }
            }
            if (!screen_off) {
            draw_playback(current_meta, elapsed_seconds);
            }
        }

        // --------------- MANEJO DE EVENTOS SDL ---------------
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (e.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_DPAD_UP: // 11 dpup
                        if (mode == MODE_LIST) {
                            if (selected_index > 0) selected_index--;
                            draw_list();
                        }
                        break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: // 12 dpdown
                        if (mode == MODE_LIST) {
                            if (selected_index < (int)entries.size() - 1) selected_index++;
                            draw_list();
                        }
                        break;
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: // 13 dpleft
                        if (mode == MODE_PLAYBACK && !screen_off) {
                            manual_switch = true; manual_forward = false; kill_playgsf();
                        }
                        break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: // 14 dpright
                        if (mode == MODE_PLAYBACK && !screen_off) {
                            manual_switch = true; manual_forward = true; kill_playgsf();
                        }
                        break;
                    default:
                        break; // Ignorar otros botones gamecontroller
                }
            }
            else if (e.type == SDL_JOYBUTTONDOWN) {
                // Mapeo basado en botones Joystick detectados
                switch (e.jbutton.button) {
                    case 0: // Botón A físico
                        if (mode == MODE_LIST) {
                            if (selected_index >= 0 && selected_index < (int)entries.size()) {
                                Entry& sel = entries[selected_index];
                                if (sel.is_dir) {
                                    current_path += (current_path == "/" ? "" : "/") + sel.name;
                                    list_directory(current_path, true);
                                    draw_list();
                                } else {
                                    std::string filepath = current_path + "/" + sel.name;
                                    if (read_metadata(filepath, current_meta)) {
                                        scroll_start_time_game = 0;
                                        scroll_start_time_title = 0;
                                        scroll_start_time_artist = 0;
                                        track_seconds = parse_length(current_meta.length);
                                        playback_start = clock_type::now();
                                        draw_playback(current_meta, 0);
                                        launch_playgsf(filepath);
                                        mode = MODE_PLAYBACK;
                                        paused = false;
                                    }
                                }
                            }
                        }
                        break;
                    case 1: // Botón B físico
                        if (mode == MODE_PLAYBACK) {
                            if (!screen_off) {
                            kill_playgsf();
                            mode = MODE_LIST;
                            draw_list();
                            }
                        } else if (mode == MODE_LIST) {
                            if (current_path != MUSIC_ROOT) {
                                size_t pos = current_path.find_last_of('/');
                                std::string last_folder = (pos != std::string::npos) ? current_path.substr(pos + 1) : current_path;
                                current_path = (pos == std::string::npos || current_path == MUSIC_ROOT) ? MUSIC_ROOT : current_path.substr(0, pos);
                                list_directory(current_path, true);
                                for (size_t i = 0; i < entries.size(); i++) {
                                    if (entries[i].is_dir && entries[i].name == last_folder) {
                                        selected_index = (int)i;
                                        break;
                                    }
                                }
                                draw_list();
                            }
                        }
                        break;
                    case 2: // Botón Y físico
                        if (mode == MODE_PLAYBACK) {
                            if (!screen_off) {
                            loop_mode = static_cast<LoopMode>((loop_mode + 1) % 3);
                            draw_playback(current_meta, elapsed_seconds);
                            }
                        }
                        break;
                    case 3: // Botón X físico
                        if (mode == MODE_PLAYBACK) {
                            bass_enabled_local = !bass_enabled_local;
                            if (playgsf_pid > 0) kill(playgsf_pid, SIGUSR2);
                            if (!screen_off) {
                            draw_playback(current_meta, elapsed_seconds);
                            }
                        }
                        break;
                    case 4: // L1 físico
                        if (mode == MODE_LIST) {
                            selected_index -= 10;
                            if (selected_index < 0) selected_index = 0;
                            draw_list();
                        }
                        break;
                    case 5: // R1 físico
                        if (mode == MODE_LIST) {
                            selected_index += 10;
                            if (selected_index >= (int)entries.size()) selected_index = (int)entries.size() - 1;
                            draw_list();
                        }
                        break;
                    case 6: // SELECT físico
                        if (!screen_off) {
                        running = false;
                        }
                        break;
                    case 7: // START físico
                        if (playgsf_pid > 0) {
                            if (!screen_off) {
                            if (!paused) {
                                kill(playgsf_pid, SIGSTOP);
                                paused = true;
                                paused_at = clock_type::now();
                            } else {
                                kill(playgsf_pid, SIGCONT);
                                paused = false;
                                auto now_chrono = clock_type::now();
                                paused_seconds_total += std::chrono::duration_cast<std::chrono::seconds>(now_chrono - paused_at).count();
                            }
                            draw_playback(current_meta, elapsed_seconds);
                            }
                        }
                        break;
                    case 8: // MENU físico (pulsacion larga)
                        //nothing
                        break;
                    case 9: // L2 físico
                        if (playgsf_pid > 0 && mode == MODE_PLAYBACK) {
                            manual_switch = true;
                            manual_forward = false;
                            kill_playgsf();
                        }
                        break;
                    case 10: // R2 físico
                        if (playgsf_pid > 0 && mode == MODE_PLAYBACK) {
                            manual_switch = true;
                            manual_forward = true;
                            kill_playgsf();
                        }
                        break;
                    case 11: // Botón extra menú o guía físico
                        // Apagar/encender pantalla o abrir menú
                        if (!screen_off) {
                            //system("wlr-randr --output DSI-1 --off");
                            //FILE* f = fopen("/sys/class/backlight/backlight/bl_power", "w");
                            //if (f) { fprintf(f, "1\n"); fclose(f); }
							render_black_screen();
							hw_display_off();
                            screen_off = true;
                        } else {
                            //system("wlr-randr --output DSI-1 --on");
                            //FILE* f = fopen("/sys/class/backlight/backlight/bl_power", "w");
                            //if (f) { fprintf(f, "0\n"); fclose(f); }
							hw_display_on();
                            screen_off = false;
                            if (mode == MODE_LIST) draw_list();
                            else draw_playback(current_meta, elapsed_seconds);
                        }
                        break;
                    default:
                        break;
                }
            }
        }
    }

    kill_playgsf();
    
    {
        std::ofstream ofs(state_file_path());
        if (ofs) {
            ofs << current_path << "\n";
            if (!entries.empty() && selected_index >= 0 && selected_index < (int)entries.size()) {
                ofs << entries[selected_index].name << "\n";
                ofs << (entries[selected_index].is_dir ? "DIR" : "FILE") << "\n";
            } else {
                ofs << "\n\n";
            }
            ofs << (bass_enabled_local ? "1" : "0") << "\n";
        }
    }
    
    if (controller) SDL_GameControllerClose(controller);
    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
