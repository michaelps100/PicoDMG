#include <emu/emulator.hpp>
#include <p8/parser.hpp>
#include <platform/platform.hpp>
#include <platform/picodmg/pico8_menu.h>
#include <platform/picodmg/mk_st7789.h>
#include <dmg_menu.h>
#include <dmg_combo.h>
#include <dmg_resume.h>
#include <dmg_boot_intro.h>
#include <hardware/watchdog.h>
#include <pico/stdlib.h>
#include <devices/sfx.hpp>
#include <devices/music.hpp>
#include <platform/picodmg/p8_audio.h>
#include <cstdio>
#include <ldo.hpp>

namespace pico = arch::pico;

int main() {
    pico::platform::init_hardware();

    char selected_cart[256];
    selected_cart[0] = '\0';

    /* Standby by save state: a game wake skips the selector and plays the
     * bootloader's intro on core 1 while the cart loads; a menu wake just plays
     * the intro before the list. */
    bool resume_game = false;
    {
        dmg_resume_record_t rec;
        const dmg_resume_kind_t rk = dmg_resume_check(pico8_menu_get_desc(), &rec);
        if (rk == DMG_RESUME_GAME) {
            snprintf(selected_cart, sizeof(selected_cart), "%s", rec.rom_file);
            dmg_boot_intro_start(pico8_menu_get_desc());
            resume_game = true;
        } else if (rk == DMG_RESUME_MENU) {
            dmg_resume_consume();
            if (dmg_boot_intro_start(pico8_menu_get_desc())) dmg_boot_intro_finish();
        }
    }

    while (true) {

        /* Open Clean Light UI ROM selector */
        /* A standby wake goes straight to the saved cart. */
        bool chosen = resume_game || pico8_select_rom(selected_cart, sizeof(selected_cart));
        if (resume_game) {
            resume_game = false;
            pico8_menu_set_resume_pending(true);
        }
        if (!chosen || selected_cart[0] == '\0') {
            /* If launched from bootloader and cancelled, return to bootloader */
            if (dmg_is_launched_from_bootloader()) {
                dmg_reboot_to_bootloader();
            }
            continue;
        }

        emu::emulator.clear_breadcrumb();
        emu::emulator.clear_pending_load_cart();

run_selected_cartridge:
        printf("[Pico8] Selected cart: %s\n", selected_cart);
        pico8_menu_set_current_game(selected_cart);

        /* Clear screen to remove selector UI and show status */
        pico8_show_status("Carregando cartucho...", 0x07E0);

        /* Reset emulator state for clean start before loading */
        emu::emulator.init();
        p8_audio_reset();
        p8_audio_set_memory(
            emu::emulator.memory().raw_view.data() + devices::Sfx::default_map_address,
            emu::emulator.memory().raw_view.data() + devices::Music::default_map_address
        );
        p8_audio_resume();

        /* Load cartridge into memory */
        emu::g_last_panic_msg[0] = '\0';
        bool load_ok = false;
        try {
            load_ok = emu::emulator.load_from_path(selected_cart);
        } catch (const EmulatorPanic &e) {
            printf("[Pico8] Panic ao carregar: %s\n", emu::g_last_panic_msg);
            load_ok = false;
        } catch (const std::exception &e) {
            printf("[Pico8] Excecao ao carregar: %s\n", e.what());
            std::strncpy(emu::g_last_panic_msg, e.what(), sizeof(emu::g_last_panic_msg) - 1);
            load_ok = false;
        } catch (...) {
            printf("[Pico8] Excecao generica ao carregar\n");
            if (emu::g_last_panic_msg[0] == '\0') {
                std::strncpy(emu::g_last_panic_msg, "Erro desconhecido", sizeof(emu::g_last_panic_msg) - 1);
            }
            load_ok = false;
        }

        /* Resume intro done (no-op otherwise) before anything else is drawn. */
        dmg_boot_intro_finish();

        if (!load_ok) {
            pico8_menu_set_resume_pending(false);
            dmg_resume_consume();
            printf("[Pico8] Failed to load cart: %s (%s)\n", selected_cart, emu::g_last_panic_msg);
            const char *err_msg = emu::g_last_panic_msg[0] ? emu::g_last_panic_msg : selected_cart;
            pico8_show_error("ERRO AO CARREGAR:", err_msg);
            continue;
        }

        /* Clear screen to black before gameplay begins */
        mk_ili9225_fill(0x0000);

        /* Clear buttons and latches before gameplay */
        dmg_wait_for_all_buttons_released();
        dmg_combo_reset();

        /* Run cartridge */
        bool restart_cart = false;
        dmg_power_set_game(selected_cart, pico8_menu_get_current_game());
        try {
            emu::emulator.run_until_shutdown();
        } catch (const EmulatorReset &e) {
            printf("[Pico8] Reset solicitado pelo menu in-game\n");
            restart_cart = true;
        } catch (const EmulatorExit &e) {
            printf("[Pico8] Cartucho finalizado, voltando a lista de jogos\n");
        } catch (const EmulatorPanic &e) {
            printf("[Pico8] Panic na execucao: %s\n", emu::g_last_panic_msg);
            pico8_show_error("ERRO NA EXECUCAO:", emu::g_last_panic_msg[0] ? emu::g_last_panic_msg : "Panic PICO-8");
        } catch (const std::exception &e) {
            printf("[Pico8] Excecao na execucao: %s\n", e.what());
            pico8_show_error("ERRO NA EXECUCAO:", e.what());
        } catch (...) {
            printf("[Pico8] Excecao generica na execucao\n");
            pico8_show_error("ERRO NA EXECUCAO:", emu::g_last_panic_msg[0] ? emu::g_last_panic_msg : "Erro desconhecido");
        }

        dmg_power_set_game(NULL, NULL);

        /* Reset emulator and audio state for next cartridge */
        p8_audio_reset();
        p8_audio_pause();
        emu::emulator.init();
        mk_ili9225_fill(0x0000);
        dmg_wait_for_all_buttons_released();
        dmg_combo_reset();

        if (restart_cart) {
            if (emu::emulator.has_pending_load_cart()) {
                std::string_view pending = emu::emulator.get_pending_load_cart();
                if (pending[0] == '/' || (pending.size() > 1 && pending[1] == ':')) {
                    snprintf(selected_cart, sizeof(selected_cart), "%.*s", (int)pending.size(), pending.data());
                } else {
                    char dir[256];
                    snprintf(dir, sizeof(dir), "%s", selected_cart);
                    char *slash = strrchr(dir, '/');
                    if (!slash) slash = strrchr(dir, '\\');
                    if (slash) {
                        *slash = '\0';
                        snprintf(selected_cart, sizeof(selected_cart), "%s/%.*s", dir, (int)pending.size(), pending.data());
                    } else {
                        snprintf(selected_cart, sizeof(selected_cart), "%.*s", (int)pending.size(), pending.data());
                    }
                }
                emu::emulator.clear_pending_load_cart();
            }
            goto run_selected_cartridge;
        }
    }
}