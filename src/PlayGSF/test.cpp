#include <fstream>
#include <thread>
#include <chrono>
#include <iostream>

void set_brightness(int level) {
    const std::string path = "/sys/class/power_supply/axp2202-battery/device/power_supply/axp2202-battery/brightness";
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "Error al abrir archivo de brillo, falta permiso o ruta incorrecta.\n";
        return;
    }
    file << level;
    file.close();
    std::cout << "Brillo seteado a " << level << std::endl;
}

int main() {
    std::cout << "Apagando pantalla...\n";
    set_brightness(0);
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::cout << "Encendiendo pantalla...\n";
    set_brightness(2);
    return 0;
}
