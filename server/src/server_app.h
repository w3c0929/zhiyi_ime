// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_SERVER_APP_H_
#define CXXIME_SERVER_APP_H_

#include <cstdint>
#include <map>
#include <string>

#include <windows.h>

#include <cxxime/control_server.h>
#include <cxxime/dictionary_monitor.h>
#include <cxxime/ipc_protocol.h>
#include <cxxime/ipc_server.h>

#include "ai_action_service.h"
#include "config_store.h"
#include "config_write_coordinator.h"
#include "candidate_preference_save_worker.h"
#include "diagnostic_log_maintenance_worker.h"
#include "input_method_hotkey.h"
#include "session_manager.h"
#include "system_lifecycle_monitor.h"
#include "translate_service.h"
#include "ui_presentation_controller.h"
#include "ui_presentation_router.h"
#include "voice_service.h"

class ServerApp {
public:
    bool initialize(const std::string& dict_path = "", const std::string& config_path = "");
    void run();
    void finalize();

private:
    cxxime::IPCResponse handle_request(const cxxime::IPCRequest& request);
    void prepare_user_data_shutdown();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // Long text over IPC (ipc_protocol.h, TEXT_UPLOAD_*): the pieces of a text that did not fit
    // translate_text, per session, and what it is for. False when `request` is not one of them.
    bool handle_text_command(const cxxime::IPCRequest& request, cxxime::IPCResponse& response);
    void finish_text_upload(uint32_t session_id);

    SessionManager session_mgr_;
    ConfigStore config_store_;
    ConfigWriteCoordinator config_writer_;
    CandidatePreferenceSaveWorker candidate_preference_saver_;
    cxxime::IpcServer ipc_server_;
    cxxime::ControlServer control_server_;
    cxxime::DictionaryMonitor dictionary_monitor_;
    DiagnosticLogMaintenanceWorker diagnostic_log_maintenance_;
    InputMethodHotkey input_method_hotkey_;
    SystemLifecycleMonitor system_lifecycle_monitor_;
    UiPresentationController ui_presentation_controller_;
    UiPresentationRouter ui_presentation_router_;
    VoiceService voice_;
    TranslateService translate_;
    AiActionService ai_action_;
    struct TextUpload {
        uint32_t purpose = 0;
        std::string action_id;  // AI actions: what to run once the text arrived
        uint64_t expected_bytes = 0;
        std::string text;
    };
    std::map<uint32_t, TextUpload> text_uploads_;
    HWND hwnd_ = nullptr;
    bool user_data_shutdown_prepared_ = false;
    std::string config_path_;
};

#endif // CXXIME_SERVER_APP_H_
