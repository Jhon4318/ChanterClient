#include "client.h"
#include "utils/logger.h"
#include <Windows.h>

DWORD WINAPI main_thread(LPVOID param) {
    const auto module = static_cast<HMODULE>(param);

    Client::instance().initialize(module);

    while (!(GetAsyncKeyState(VK_END) & 1)) {
        Sleep(50);
    }

    LOG_INFO("END pressed. Removing hooks and unloading ChanterClient...");
    Client::instance().shutdown();

    // Give the render hook a brief opportunity to leave its current callback.
    Sleep(150);
    FreeLibraryAndExitThread(module, 0);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);

        HANDLE thread = CreateThread(nullptr, 0, main_thread, module, 0, nullptr);
        if (!thread) {
            return FALSE;
        }
        CloseHandle(thread);
    }

    return TRUE;
}
