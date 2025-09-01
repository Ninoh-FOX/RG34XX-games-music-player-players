#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <string.h>

#define DEV_INPUT "/dev/input/event1"
#define KEY_VOLUP 115
#define KEY_VOLDOWN 114
#define MAX_STEPS 10
#define MAX_VOLUME 31  // escala real del hardware para 'lineout volume'

// Convierte paso 0-10 a valor real 0-31
int stepToVolume(int step) {
    if (step < 0) step = 0;
    if (step > MAX_STEPS) step = MAX_STEPS;
    return (step * MAX_VOLUME) / MAX_STEPS;
}

// Obtiene el volumen actual como paso 0-10
int getVolumeStep() {
    FILE *pipe = popen("amixer get 'lineout volume' | grep -o '[0-9]*%' | head -1", "r");
    if (!pipe) return -1;
    char buf[16];
    if (fgets(buf, sizeof(buf), pipe) != NULL) {
        int percent = 0;
        sscanf(buf, "%d", &percent);
        pclose(pipe);
        int volume = (percent * MAX_VOLUME) / 100;
        int step = (volume * MAX_STEPS + MAX_VOLUME/2) / MAX_VOLUME; // redondeo
        if (step < 0) step = 0;
        if (step > MAX_STEPS) step = MAX_STEPS;
        return step;
    }
    pclose(pipe);
    return -1;
}

// Establece volumen desde paso 0-10
void setVolumeStep(int step) {
    if (step < 0) step = 0;
    if (step > MAX_STEPS) step = MAX_STEPS;
    int vol = stepToVolume(step);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "amixer set 'lineout volume' %d", vol);
    system(cmd);
}

int main() {
    int fd = open(DEV_INPUT, O_RDONLY);
    if (fd < 0) {
        perror("Error abriendo dispositivo de input");
        return 1;
    }

    int step = getVolumeStep();
    if (step < 0) {
        fprintf(stderr, "No se pudo obtener el volumen inicial, iniciando en 5\n");
        step = 5;
        setVolumeStep(step);
    }

    printf("Volumen inicial (pasos 0-10): %d\n", step);

    struct input_event ev;
    while (1) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != sizeof(ev)) continue;

        if (ev.type == EV_KEY && ev.value == 1) { // PRESSED
            if (ev.code == KEY_VOLUP) {
                if (step < MAX_STEPS) {
                    step++;
                    setVolumeStep(step);
                    printf("Volumen subido a paso %d\n", step);
                }
            } else if (ev.code == KEY_VOLDOWN) {
                if (step > 0) {
                    step--;
                    setVolumeStep(step);
                    printf("Volumen bajado a paso %d\n", step);
                }
            }
        }
    }

    close(fd);
    return 0;
}
