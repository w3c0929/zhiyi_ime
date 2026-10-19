// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TSF_TEXT_SERVICE_H_
#define CXXIME_TSF_TEXT_SERVICE_H_

class CandidateUIElement;
class ReadingUIElement;

#include "pch.h"

#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <cxxime/ai_actions.h>
#include <cxxime/config.h>
#include <cxxime/control_protocol.h>
#include <cxxime/ime_menu.h>
#include <cxxime/ipc_client.h>
#include <cxxime/ipc_protocol.h>
#include <cxxime/host_trace.h>
#include <cxxime/ui_channel.h>

#include "candidate_presentation.h"
#include "edit_target.h"
#include "effective_edit_target.h"
#include "input_indicator_controller.h"

namespace cxxime {
class CandidateWindow;
}

namespace cxxime_tsf {

bool is_valid_caret_rect(const RECT& rect);
class UiPresentationBatch;

}  // namespace cxxime_tsf

class TextService : public ITfTextInputProcessorEx,
                    public ITfKeyEventSink,
                    public ITfCompositionSink,
                    public ITfThreadFocusSink,
                    public ITfThreadMgrEventSink,
                    public ITfCompartmentEventSink,
                    public ITfTextEditSink,
                    public ITfTextLayoutSink,
                    public ITfDisplayAttributeProvider,
                    public ITfFunctionProvider,
                    public ITfFnSearchCandidateProvider {
public:
    TextService();
    ~TextService();

    // The mark on the keys press_copy() sends a program that cannot be read
    // (text_service_translate.cpp). The input method's own key handlers skip keys carrying it: they
    // are the program's to receive, and the 'C' among them would otherwise close the menu that is
    // opening.
    static constexpr LPARAM kInjectedCopyMark = 0x5A59'434F;  // 'ZYCO'

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfTextInputProcessorEx
    STDMETHODIMP Activate(ITfThreadMgr* ptim, TfClientId tid) override;
    STDMETHODIMP Deactivate() override;
    STDMETHODIMP ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) override;

    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL fForeground) override;
    STDMETHODIMP OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnPreservedKey(ITfContext* pic, REFGUID rguid, BOOL* pfEaten) override;

    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) override;

    // ITfThreadFocusSink
    STDMETHODIMP OnSetThreadFocus() override;
    STDMETHODIMP OnKillThreadFocus() override;

    // ITfThreadMgrEventSink
    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* pDocMgr) override;
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* pDocMgr) override;
    STDMETHODIMP OnSetFocus(ITfDocumentMgr* pDocMgrFocus, ITfDocumentMgr* pDocMgrPrevFocus) override;
    STDMETHODIMP OnPushContext(ITfContext* pic) override;
    STDMETHODIMP OnPopContext(ITfContext* pic) override;

    // ITfCompartmentEventSink
    STDMETHODIMP OnChange(REFGUID rguid) override;

    // ITfTextEditSink
    STDMETHODIMP OnEndEdit(ITfContext* pic,
                           TfEditCookie ecReadOnly,
                           ITfEditRecord* editRecord) override;

    // ITfTextLayoutSink
    STDMETHODIMP OnLayoutChange(ITfContext* pic,
                                TfLayoutCode lcode,
                                ITfContextView* view) override;

    // ITfDisplayAttributeProvider
    STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) override;
    STDMETHODIMP GetDisplayAttributeInfo(REFGUID rguid, ITfDisplayAttributeInfo** ppInfo) override;

    // ITfFunctionProvider
    STDMETHODIMP GetType(GUID* guid) override;
    STDMETHODIMP GetDescription(BSTR* description) override;
    STDMETHODIMP GetFunction(REFGUID function_guid, REFIID riid, IUnknown** function) override;

    // ITfFnSearchCandidateProvider
    STDMETHODIMP GetDisplayName(BSTR* name) override;
    STDMETHODIMP GetSearchCandidates(BSTR query,
                                     BSTR application_id,
                                     ITfCandidateList** candidates) override;
    STDMETHODIMP SetResult(BSTR query, BSTR application_id, BSTR result) override;

    // Helper. `under_selection` writes the text after the selection instead of over it — an AI
    // action whose insert mode is "below" (a summary belongs under the text it summarizes).
    // `as_correction` marks the edit as correcting what it replaces, which is what a composition's
    // own commit wants; an AI answer passes false so that the program sees an ordinary edit, undone
    // by its own Ctrl+Z (docs/ai.md).
    HRESULT insert_text(const std::wstring& text, bool sync = false,
                        uint64_t* request_generation = nullptr, bool under_selection = false,
                        bool as_correction = true);
    bool select_candidate_from_ui(UINT index, uint64_t candidate_revision);
    bool navigate_candidate_page_from_ui(bool previous);
    // The server's candidates changed without a key (UiCommandType::kRefreshCandidates).
    bool refresh_candidates_from_server();
    void abort_candidate_ui_from_tsf();
    HRESULT finalize_exact_candidate_ui_from_tsf();
    bool set_candidate_ui_element_shown(bool show);
    bool is_candidate_ui_element_shown() const;
    void trace_ui_element_method(const char* element, const char* method, bool important = false);
    void trace_candidate_activation_state(ITfDocumentMgr* candidate_document_mgr) const;
    uint64_t trace_input_id() const { return _hostTraceSession.input_id(); }
    uint64_t trace_composition_id() const { return _hostTraceSession.composition_id(); }
    uint64_t ensure_trace_composition_id();
    void trace_caret_event(const char* action,
                           const char* source,
                           bool resolved,
                           const RECT* rect,
                           HRESULT hr = S_OK,
                           bool important = false);
    void trace_text_ext_rect(const cxxime_tsf::TextExtRectTrace& trace);
    HRESULT update_composition(ITfContext* pic,
                               const std::wstring& preedit,
                               size_t preedit_cursor,
                               bool ensure = false,
                               DWORD edit_session_mode = TF_ES_ASYNCDONTCARE,
                               size_t converted_prefix_utf16 = 0,
                               size_t focused_start_utf16 = 0,
                               size_t focused_end_utf16 = 0,
                               bool focused_converted = false,
                               const std::optional<std::wstring>& host_termination_text =
                                   std::nullopt,
                               bool* host_edit_started = nullptr,
                               uint64_t* request_generation = nullptr,
                               const std::wstring& commit_before_preedit = L"");
    bool apply_composition_display_attributes(ITfContext* pic,
                                              ITfRange* range,
                                              TfEditCookie ec,
                                              size_t converted_prefix_utf16,
                                              size_t focused_start_utf16,
                                              size_t focused_end_utf16,
                                              bool focused_converted);
    ITfComposition* get_composition() const { return _composition; }
    void set_composition(ITfComposition* comp) { _composition = comp; }
    ITfContext* get_composition_context() const { return _compositionContext; }
    bool is_composing() const { return _composing; }
    bool is_immersive_mode() const {
        return (_activateFlags & TF_TMF_IMMERSIVEMODE) != 0;
    }
    void set_composition_context(ITfContext* context);
    void set_composing(bool val) {
        _composing = val;
        if (val) _install_translation_key_hook();
    }
    bool empty_composition_placeholder_active() const {
        return _emptyCompositionPlaceholderActive;
    }
    void set_empty_composition_placeholder_active(bool active) {
        _emptyCompositionPlaceholderActive = active;
    }
    void set_applied_inline_composition_text(
        const std::wstring& text,
        const std::optional<std::wstring>& host_termination_text = std::nullopt) {
        _lastInlineCompositionText = text;
        _hostTerminationCompositionText =
            host_termination_text && text != *host_termination_text
                ? host_termination_text
                : std::nullopt;
    }
    void clear_applied_inline_composition_text() {
        set_applied_inline_composition_text(L"");
    }
    bool composition_requires_placeholder(const std::wstring& next_text) const;
    void set_caret_rect(const RECT& rc) {
        _caretRect = rc;
        _caretRectTargetGeneration = _uiTargetGeneration;
        ++_caretSampleSerial;
    }
    void remember_viewport_caret(const RECT& view_rect, const RECT& caret_rect) {
        _caretViewportTracker.remember(_uiTargetGeneration, view_rect, caret_rect);
    }
    cxxime_tsf::CaretViewportFallback resolve_viewport_caret(const RECT* view_rect,
                                                             const RECT* logical_rect, bool clipped,
                                                             RECT* caret_rect) const {
        return _caretViewportTracker.resolve(_uiTargetGeneration, view_rect, logical_rect, clipped,
                                             caret_rect);
    }
    void update_candidate_position(const RECT& rc,
                                   ITfContext* context = nullptr,
                                   bool from_layout_change = false,
                                   uint64_t expected_generation = 0,
                                   bool viewport_fallback = false);
    bool composition_edit_request_is_current(
        uint64_t expected_generation, uintptr_t expected_context_identity) const;
    uint64_t begin_composition_edit_request();
    uint64_t invalidate_composition_edit_requests();
    void register_composition_edit() { ++_pendingCompositionEdits; }
    void release_composition_edit() { --_pendingCompositionEdits; }
    DWORD ordered_composition_edit_mode(DWORD requested) const {
        return _pendingCompositionEdits > 1 ? TF_ES_ASYNC : requested;
    }
    void handle_composition_restart_success(uint64_t expected_generation);
    bool handle_composition_edit_failure(uint64_t expected_generation,
                                         uintptr_t expected_context_identity);
    RECT _resolve_caret_rect(ITfContext* pic);

    // TSF layer trace (lightweight, no cross-module QueryTrace dependency)
    enum class TsfResult : uint8_t {
        IPC_FAILED = 0,
        COMMITTED,
        PREEDIT,
        CLEARED,
        HANDLED,
        REJECTED,
    };

    struct TsfTrace {
        uint32_t vk = 0;
        uint32_t modifiers = 0;
        TsfResult result = TsfResult::REJECTED;
        uint32_t candidate_count = 0;
        uint32_t preedit_len = 0;
        uint32_t preedit_cursor = 0;
        int64_t total_us = 0;
        int64_t ipc_us = 0;
        int64_t window_us = 0;
        bool slow = false;

        const char* result_string() const;
        int to_json(char* buf, int size) const;
        bool should_log() const;
    };

private:
    friend class cxxime_tsf::UiPresentationBatch;
    friend struct TextServiceTestPeer;

    HRESULT _initialize_required_activation_sinks();
    void _initialize_optional_activation_services();
    void _synchronize_activation_focus();
    HRESULT _register_key_event_sink();
    HRESULT _unregister_key_event_sink();
    // The configured switch shortcuts are preserved keys. Where Windows' own input method
    // hotkeys still take the key first (Ctrl+Space open/close, Shift+Space full/half width,
    // Ctrl+. punctuation, in plain edit boxes), only the open/close or conversion compartment
    // changes: the switch key held at that moment is then run here
    // (_run_held_switch_key, text_service_compartment.cpp).
    void _register_switch_keys();
    void _unregister_switch_keys();
    void _register_open_close_compartment_sink();
    void _unregister_open_close_compartment_sink();
    // The configured switch key (c_guidPreservedKey* order) held now, else -1.
    int _held_switch_key() const;
    // True once per press: false when the same switch key was run in the last moment (the
    // key event, the preserved key and a system hotkey can all report one press).
    bool _claim_switch_key_press(int slot);
    bool _run_held_switch_key();
    // Chinese/English, punctuation or full/half width switched directly (no input box
    // needed); false for the style key or when the server did not answer.
    bool _apply_switch_key(int slot);
    // The style key (slot 1) or the voice key (slot 4) taken by a system hotkey (Ctrl+Space by
    // default for the style key): sent to the server as a whole press, so it does what the key
    // does when it reaches the IME; false when the server did not answer.
    bool _press_switch_key(int slot);
    // Voice input (text_service_voice.cpp): asking the server where the recording is while it
    // records and recognizes, and the text it gives, shown underlined until a key decides.
    void _start_voice_poll();
    void _stop_voice_poll(bool cancel);
    void _poll_voice();
    void _show_voice_text(const std::wstring& text);
    void _commit_voice_text(ITfContext* context);
    void _delete_voice_text(ITfContext* context);
    // A key while the voice text is shown: true when the key was used up (Backspace, Esc,
    // Enter); otherwise the text is kept and the key goes on as usual.
    bool _handle_voice_text_key(ITfContext* context, WPARAM key);
    // Windows keeps a Chinese/English mode per input box (the IMM layer) and restores it when
    // the box gets focus. Like Microsoft Pinyin, the IME keeps one mode everywhere: such a
    // restore is written back over; a change by a program at another time is applied.
    void _settle_conversion_change();
    void _note_focus_change() { _focusChangeTick = GetTickCount64(); }
    // Input collection (second tier of the experience program): tells the server the program
    // and window title of the input when they changed; nothing while not allowed.
    void _report_input_target();
    // Before the first key of a composition: the text before the caret becomes the
    // recommendation model's context (SET_CONTEXT).
    void _send_text_before_caret(ITfContext* context);
    HRESULT _register_thread_mgr_event_sink();
    HRESULT _register_thread_focus_sink();
    void _unregister_thread_sinks();
    void _register_conversion_compartment_sink();
    void _unregister_conversion_compartment_sink();
    bool _register_display_attribute_atom();
    HRESULT _end_composition(ITfContext* pic, bool sync = false);
    HRESULT _commit_text(ITfContext* pic, const std::wstring& text, bool sync = false,
                         uint64_t* request_generation = nullptr);
    HRESULT _commit_then_restart_composition(ITfContext* pic,
                                             const std::wstring& commit_text,
                                             const std::wstring& preedit,
                                             size_t preedit_cursor,
                                             size_t converted_prefix_utf16,
                                             size_t focused_start_utf16,
                                             size_t focused_end_utf16,
                                             bool focused_converted,
                                             const std::optional<std::wstring>&
                                                 host_termination_text,
                                             uint64_t* request_generation = nullptr);
    bool _apply_engine_response(ITfContext* context,
                                const cxxime::IPCResponse& response,
                                BOOL* eaten,
                                TsfTrace* trace = nullptr);
    bool _ProcessKeyEvent(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten);
    bool _ProcessKeyUp(WPARAM wParam, LPARAM lParam);
    void _AbortComposition();
    void _reset_trace_composition(const char* reason);
    ITfContext* _current_edit_context_for_composition() const;
    uint32_t _get_modifiers() const;
    // Learning mode: Ctrl+1..9 types a candidate's translation (docs/learning-mode.md). Eaten
    // keys still reach Chromium apps as "processed" keys with their key code, which some (the
    // Claude app) take as their own Ctrl+1..9; while typing, a low-level keyboard hook takes the
    // keys before any program gets them and hands them to this text service.
    bool _wants_translation_keys() const;
    void _install_translation_key_hook();
    void _remove_translation_key_hook();
    bool _take_translation_key(UINT message, const KBDLLHOOKSTRUCT& key);
    void _process_translation_key(UINT vk, bool key_up);
    static LRESULT CALLBACK _translation_key_hook_proc(int code, WPARAM wp, LPARAM lp);
    bool _is_caps_lock_on() const;
    void _sync_ime_status(const cxxime::ImeStatus& status);
    void _handle_ime_menu_command(cxxime::ImeMenuCommand command);
    void _sync_conversion_mode_compartment(const cxxime::ImeStatus& status);
    HRESULT _read_conversion_mode_compartment(DWORD* conversion_mode,
                                              VARTYPE* value_type) const;
    bool _context_belongs_to_foreground(ITfContext* context) const;
    bool _context_matches_effective_edit_target(ITfContext* context) const;
    bool _ensure_text_edit_sink(ITfContext* context);
    bool _bind_text_edit_sink(ITfContext* context);
    void _unadvise_text_edit_sink();
    bool _bind_text_layout_sink(ITfContext* context);
    void _unadvise_text_layout_sink();
    void _request_candidate_position_update(ITfContext* pic,
                                            const char* reason,
                                            bool from_layout_change = false);
    void _follow_native_caret();
    bool _resolve_native_caret_rect(RECT* out) const;
    bool _resolve_context_native_caret_rect(ITfContext* context,
                                            RECT* out,
                                            HWND* context_window = nullptr) const;
    bool _read_context_compartment_bool(ITfContext* context, REFGUID guid, bool* value) const;
    bool _context_keyboard_disabled(ITfContext* context) const;
    const char* _input_context_block_reason(ITfContext* context) const;
    bool _context_allows_input(ITfContext* context) const;
    bool _document_allows_input(ITfDocumentMgr* doc_mgr) const;
    bool _query_input_focus_from_thread_mgr() const;
    bool _synchronize_effective_edit_target(ITfContext* event_context,
                                            ITfDocumentMgr* event_document_mgr,
                                            const char* source);
    bool _synchronize_effective_edit_target_from_thread_mgr(const char* source);
    void _clear_effective_edit_target(const char* source, bool target_unavailable = false);
    void _release_effective_edit_target();
    cxxime_tsf::EffectiveEditTargetBindings _effective_edit_target_bindings(
        const cxxime_tsf::EffectiveEditTargetSnapshot& target) const;
    void _trace_effective_edit_target_sync(
        const char* source,
        cxxime_tsf::EffectiveEditTargetAction action,
        const cxxime_tsf::EffectiveEditTargetSnapshot& previous,
        const cxxime_tsf::EffectiveEditTargetSnapshot& next,
        const cxxime_tsf::EffectiveEditTargetBindings& bindings,
        bool succeeded);
    bool _sync_caps_lock_state(bool caps_lock,
                               const char* source,
                               cxxime::ImeStatus* synced_status = nullptr);
    bool _refresh_caps_lock_on_focus(const char* source);
    void _schedule_caps_lock_refresh();
    bool _ensure_ipc_session();
    bool _recreate_ipc_session_preserving_status();
    bool _heartbeat_ipc();
    bool _refresh_input_indicator();
    void _schedule_input_indicator_refresh_retry();
    void _stop_input_indicator_refresh_retry();
    bool _has_synced_ime_status() const noexcept {
        return _hasLastImeStatus.load(std::memory_order_acquire);
    }
    // Publishes the UI presentation after an input target change (traced with `reason`).
    void _refresh_ui_presentation(const char* reason);
    void _show_candidate_window(const char* reason);
    void _hide_candidate_window(const char* reason);
    void _hide_candidate_projection(const char* reason);
    void _hide_external_candidate_window(const char* reason);
    bool _publish_candidate_ui_element();
    void _sync_candidate_ui_element_snapshot();
    cxxime::CandidateUiContext _candidate_ui_context() const;
    static std::wstring utf8_to_wstring(const char* text);
    void _start_host_compatibility_runtime();
    void _stop_host_compatibility_runtime();
    void _prepare_host_candidate_compatibility();
    void _update_reading_ui_element(ITfContext* context, const std::wstring& reading);
    void _end_reading_ui_element(const char* reason);
    void _trace_input_decision(const char* block_reason);
    void _update_state_poll_timer();
    void _stop_state_poll_timer();
    void _poll_runtime_state();
    static VOID CALLBACK _state_poll_timer_proc(HWND hwnd, UINT msg, UINT_PTR id_event, DWORD time);
    bool _start_config_updates();
    void _stop_config_updates();
    void _apply_config_snapshot();
    bool _start_ui_presentation_channel();
    void _stop_ui_presentation_channel();
    void _publish_ui_presentation();
    bool _present_local_candidate_window(const cxxime::CandidatePresentationPage& page,
                                         int page_current,
                                         int page_total,
                                         const std::string& preedit,
                                         std::size_t preedit_cursor, const RECT& caret);
    void _hide_local_candidate_window();
    // Selection translation (text_service_translate.cpp): the translate key was pressed, the
    // server's answer (polled), and the result window.
    // The translate key (Ctrl+C by default) pressed twice: true when the press was taken (its
    // key-up is swallowed too). Called from OnTestKeyDown and OnKeyDown.
    bool _handle_translate_key(WPARAM key);
    void _process_selection_translate();
    // AI actions on the selection (docs/ai.md): the second press of shortcuts.ai_action opens a
    // menu of the actions beside the selection; the chosen one runs through the same channel
    // (_send_selected_text) the translation uses, so the poll and the result window are shared.
    // Called from OnTestKeyDown / OnKeyDown, from where _handle_translate_key is called.
    bool _handle_ai_action_key(WPARAM key);
    // Which AI action `key` is (an empty id: the menu), and the key-up that fires a tapped
    // modifier.
    bool _ai_action_key_for(WPARAM key, std::string* action_id) const;
    bool _finish_ai_action_key(WPARAM key, BOOL* pfEaten);
    // A tapped modifier's own key state, watched on a timer: some programs never hand the text
    // service the modifier's key-up, so waiting for that event would never fire (docs/ai.md).
    void _watch_ai_action_tap();
    // Which key was tapped, and what its tap opens (docs/ai.md): an action (or the menu when its id
    // is empty), the translate language list, the voice post-processing list, or the online switch.
    enum class AiKeyPurpose { kAction, kLanguage, kVoiceMode, kOnline };
    // What the open menu does with a row (docs/ai.md): run the action, set 语音识别后处理's action,
    // or turn the whole online AI on and off.
    enum class AiMenuPurpose { kRunAction, kSetVoiceMode, kToggleOnline };
    // The menu key was taken: read the selection and open the menu (on the config window's thread,
    // like _process_selection_translate).
    void _process_ai_action_selection();
    // The key that opens the translate language list (ai.language_key): the same list \fy opens,
    // typed for the user so the engine recognizes the command (docs/ai.md).
    bool _is_language_key(WPARAM key) const;
    void _open_language_list();
    // A key that decided to fire: the language list, the voice post-processing list, or an action
    // (the menu when the id is empty).
    void _fire_ai_action_key(AiKeyPurpose purpose, const std::string& action_id);
    // A key while the menu is open: true when the menu took it (pfEaten is set then).
    bool _handle_ai_menu_key(WPARAM key, BOOL* pfEaten);
    // Only opens the menu: the text an action works on is read when a row is picked (docs/ai.md).
    void _open_ai_menu();
    bool _show_menu_window();
    void _close_ai_menu();
    void _render_ai_menu();  // redraw the rows (the highlight moved)
    void _run_ai_action(std::size_t row);  // a menu row
    void _start_ai_action(std::size_t index, const std::wstring& source, bool shortened);
    // The text to work on (docs/ai.md), in that order: the selection; with nothing selected the
    // part of the caret's line written so far — which is selected for the user and put on the
    // clipboard; and, only when neither has anything, the clipboard itself (what the user copied).
    // A program the input method cannot read is asked to copy its selection; when even that fails
    // the clipboard is refused, unless `the_user_copied` says the key's own first press was the
    // user's copy (the translate key's Ctrl+C), which is what makes that clipboard theirs.
    // Truncated at translate.max_selection_chars; false when there is nothing to work on, with
    // `reason` (when given) set to what the window should say.
    bool _read_selected_text(std::wstring* selection, bool* shortened,
                             const wchar_t** reason = nullptr, bool the_user_copied = false);
    // Sends the selected text for `purpose` (kTextUpload*): the short command when it fits the IPC
    // field, the chunked one otherwise. `insert` is what the answer does (ai_actions.h: over the
    // selection, under it, or only shown); `note` is what the window shows beside it (a shortened
    // selection, for instance).
    bool _send_selected_text(std::uint32_t purpose, const std::string& action_id,
                             const std::string& text, const std::wstring& source,
                             cxxime::AiActionInsert insert, const std::wstring& note);
    void _start_translation_poll();
    void _stop_translation_poll();
    void _poll_translation();
    void _present_translate_window(const std::wstring& translation, const std::wstring& source,
                                   const std::wstring& note, const RECT& caret);
    // A key that could not do anything says why (docs/ai.md, 无法完成：…).
    void _present_failure(const std::wstring& reason, const std::wstring& source);

    // Our own windows (the answer's, and the action menu) get a moment of grace: reading the text
    // copies it for the user, and in some editors (Sublime Text and other classic ones, especially
    // elevated) that makes the program re-activate its input context. The input method then sees
    // its edit target change — and the code that tidies up after such a change would take the
    // window down again before it could be seen (they have no window of their own to show it in,
    // which is why this shows up there and not in a browser).
    void _arm_ai_ui_grace();
    bool _ai_ui_grace_active() const;
    // The trace of where the text came from (docs/ai.md): selection, the caret's line, the
    // program's own copy, or the clipboard — with its length, and never the text itself.
    void _trace_ai_source(const char* tier, const std::wstring& text);

    void _hide_translate_window();
    // The key that opens the voice post-processing list (纠错 / 提问 / 不使用), and that list itself.
    bool _is_voice_action_key(WPARAM key) const;
    void _open_voice_mode_menu();
    // The key that opens the online switch's list (线上（开）/ 线上（关）), and that list itself: it
    // writes the same ai.enabled the settings page's master switch does (docs/ai.md).
    bool _is_enable_key(WPARAM key) const;
    void _open_online_menu();
    // The name of a menu row that is not an AI action — 不使用, 线上（开）, 线上（关）— by its number
    // after the actions (the TSF has no ui strings of its own).
    std::string _menu_special_label(std::size_t special) const;
    // The host program's file name, lowercased ("reaper.exe"), and whether it is on the
    // pass-through list (设置 > 常规 > 不接管按键的程序).
    std::string _host_program_name() const;
    void _refresh_keyboard_pass_through();
    // The caret of the focused input box, asked for now: the cached one may be stale with nothing
    // typed. The candidate window does the same before it opens.
    RECT _query_caret_rect();
    void _publish_ui_session_ended();
    void _queue_ui_command(const cxxime::UiCommand& command);
    void _drain_ui_commands();
    bool _is_current_ui_command(const cxxime::UiCommand& command) const;
    void _handle_ui_command(const cxxime::UiCommand& command);
    static LRESULT CALLBACK _config_window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    LONG _cRef = 1;
    ITfThreadMgr* _threadMgr = nullptr;
    TfClientId _clientId = TF_CLIENTID_NULL;
    ITfComposition* _composition = nullptr;
    ITfContext* _compositionContext = nullptr;
    DWORD _activateFlags = 0;
    DWORD _dwThreadFocusCookie = TF_INVALID_COOKIE;
    DWORD _dwThreadMgrEventCookie = TF_INVALID_COOKIE;
    DWORD _dwConversionCompartmentCookie = TF_INVALID_COOKIE;
    DWORD _dwTextEditSinkCookie = TF_INVALID_COOKIE;
    DWORD _dwTextLayoutSinkCookie = TF_INVALID_COOKIE;
    TfGuidAtom _displayAttributeAtom = 0;
    TfGuidAtom _convertedDisplayAttributeAtom = 0;
    TfGuidAtom _focusedDisplayAttributeAtom = 0;
    TfGuidAtom _focusedConvertedDisplayAttributeAtom = 0;
    ITfContext* _textEditSinkContext = nullptr;
    ITfContext* _textLayoutSinkContext = nullptr;
    ITfDocumentMgr* _effectiveDocumentMgr = nullptr;
    ITfContext* _effectiveContext = nullptr;
    cxxime_tsf::EffectiveEditTargetSnapshot _effectiveEditTarget;
    ITfCompartment* _conversionCompartment = nullptr;
    ITfSource* _conversionCompartmentSource = nullptr;
    ITfCompartment* _openCloseCompartment = nullptr;
    ITfSource* _openCloseCompartmentSource = nullptr;
    DWORD _dwOpenCloseCompartmentCookie = TF_INVALID_COOKIE;
    ULONGLONG _switchKeyTicks[5] = {};  // when each switch key was last run
    ULONGLONG _focusChangeTick = 0;       // the last focus change in this thread
    ULONGLONG _conversionChangeTick = 0;  // an outside conversion change waiting to settle
    bool _applyingDeferredConversion = false;

    cxxime::IpcClient _client;
    uint32_t _sessionId = 0;
    bool _composing = false;
    uint64_t _compositionEditGeneration = 0;
    unsigned int _pendingCompositionEdits = 0;
    bool _emptyCompositionPlaceholderActive = false;
    bool _chinese_mode = true;
    bool _caps_lock = false;
    std::mutex _lastImeStatusMutex;
    cxxime::ImeStatus _lastImeStatus;
    std::atomic<bool> _hasLastImeStatus{false};
    bool _activated = false;
    bool _inputFocused = false;
    bool _inputTargetUnavailable = false;
    unsigned int _runtimeTargetValidationFailures = 0;
    bool _fTestKeyDownPending = false;
    bool _fTestKeyUpPending = false;
    std::bitset<256> _passThroughKeyUps;
    bool _hostCompatibilityRuntimeActive = false;
    bool _writingConversionCompartment = false;
    bool _handlingConversionCompartmentChange = false;
    UINT_PTR _statePollTimer = 0;
    UINT _statePollIntervalMs = 0;
    std::chrono::steady_clock::time_point _lastIpcHeartbeat = {};
    bool _ipcHealthy = true;
    std::string _lastInputBlockReason;
    std::wstring _lastInlineCompositionText;
    std::wstring _reportedInputTarget;  // program + window title last sent to the server
    uintptr_t _contextReadTarget = 0;   // the input box the context was last read from
    std::optional<std::wstring> _hostTerminationCompositionText;
    cxxime_tsf::CandidatePresentation _candidatePresentation;
    CandidateUIElement* _candidateUiElement = nullptr;
    ReadingUIElement* _readingUiElement = nullptr;
    cxxime::Config _config;
    // Registered preserved switch keys, in c_guidPreservedKey* order (disabled when unset).
    cxxime::KeyboardShortcut _preservedSwitchKeys[5] = {};
    cxxime::KeyboardShortcut _switch_key(int slot) const;
    std::unique_ptr<cxxime::CandidateWindow> _localCandidateWindow;
    std::uint64_t _localCandidatePlacementTargetGeneration = 0;
    cxxime::ConfigGeneration _configGeneration;
    HWND _configWindow = nullptr;
    HHOOK _translationKeyHook = nullptr;
    bool _voicePolling = false;               // the server records or recognizes for us
    std::uint64_t _voiceTargetGeneration = 0;  // the input box it is for (_uiTargetGeneration)
    bool _voiceComposition = false;           // the voice text is shown, underlined
    std::wstring _voiceText;
    // Selection translation (text_service_translate.cpp).
    bool _translationPolling = false;         // the server translates for us
    std::uint64_t _translationTargetGeneration = 0;
    std::unique_ptr<cxxime::CandidateWindow> _translateWindow;  // shows the translation or answer
    std::wstring _translateSource;            // the selected text (shown after the translation)
    // The AI action menu: its own window (the translation window is dismissed by any key, the menu
    // has to take keys), the rows it shows (indices into cxxime::ai_actions()) and what it is for.
    // 设置 > 常规 > 不接管按键的程序 (Config::pass_through_programs): this host is on the list, so
    // every key goes straight to it — nothing is composed, no candidate window opens, no key is
    // eaten (a DAW or a game, where a single letter is a shortcut). Kept in step with the config
    // (_refresh_keyboard_pass_through).
    bool _keyboardPassThrough = false;
    bool _aiMenuOpen = false;
    std::size_t _aiMenuHighlight = 0;
    std::vector<std::size_t> _aiMenuRows;
    std::string _pendingAiActionId;  // an action's own key was pressed: run it, no menu
    bool _pendingLanguageMenu = false;   // the language key was pressed: open \fy's list
    bool _pendingVoiceModeMenu = false;  // the voice key was pressed: open the 纠错 / 提问 list
    bool _pendingOnlineMenu = false;     // the online key was pressed: open the 线上（开/关）list
    AiKeyPurpose _aiActionKeyPurpose = AiKeyPurpose::kAction;
    AiMenuPurpose _aiMenuPurpose = AiMenuPurpose::kRunAction;
    std::unique_ptr<cxxime::CandidateWindow> _aiMenuWindow;
    ULONGLONG _aiUiGraceUntil = 0;  // see _arm_ai_ui_grace
    // Long text over IPC (TEXT_UPLOAD_*): what the request in flight is for, whether it went
    // through the chunked channel, whether it may be typed back, and what to note beside it.
    std::uint32_t _textPurpose = 0;           // kTextUploadTranslate / kTextUploadAiAction
    std::string _textActionId;                // the AI action, when that is what it is
    bool _textPieces = false;                 // the answer is read in pieces (chunked channel)
    cxxime::AiActionInsert _textInsert =      // what the answer does with the selection
        cxxime::AiActionInsert::kPreview;
    // The text had to come from the program's own copy (its document could not be read): the answer
    // goes back the same way — pasted — so the program replaces what it has selected (docs/ai.md).
    bool _textProgramOwnsItsText = false;
    // The program's own copy was exactly the caret's line (break and all): it is selected before the
    // answer is pasted, so that the answer replaces the line instead of landing in the middle of it.
    bool _textProgramCopiedLine = false;
    // That line, break off: the copy the selection is checked against, so the answer is pasted over
    // it and not over whatever the caret happens to be on (docs/ai.md).
    std::wstring _textLineText;
    // After a paste: 1 when the line the answer was written into starts with it (the caret sits right
    // after what was pasted, so the read the action used holds the answer), 0 when that read shows the
    // *old* text is still there, and -1 when there was nothing to read at all — a program that will
    // not tell, where the paste is taken on trust (the retry has already run).
    int _line_starts_with(const std::wstring& expected);
    // `insert: below` in a program the input method cannot write into (Sublime Text and the like):
    // the caret is walked to the end of what was worked on with the program's own keys, and the
    // answer, newline first, is pasted there (docs/ai.md).
    void _deliver_below_by_keys(const std::wstring& block);
    // The read selected the caret's line itself (the tier-2 read: the part before the caret) in a
    // document the input method *can* read: the answer replaces it, and the delivery selects it with
    // the program's own keys, because a TSF selection is not the program's own (docs/ai.md).
    bool _textLineSelected = false;
    // A classic edit control (Edit, RichEdit) the text came from through its own messages: the
    // answer goes back the same way, and this range is what it replaces (docs/ai.md).
    HWND _textEditControl = nullptr;
    LONG _textEditStart = 0;
    LONG _textEditEnd = 0;
    // A program whose text came through UI Automation: the delivery selects the same target again and
    // checks it is still that text before pasting over it (docs/ai.md).
    bool _textUiaTarget = false;
    std::wstring _textUiaText;
    // The "正在润色…" window: what it says, and that it is the one on screen. Some editors take it
    // down behind our back while the answer is being fetched (their input context moves over the
    // copy that read the text), so it is put back on every poll (docs/ai.md).
    std::wstring _textProgressText;
    bool _textProgressShown = false;
    // What the last _read_selected_text found: the document could not be read at all, so the text
    // came from the program's own Ctrl+C (or not at all).
    bool _readNeededProgramCopy = false;
    // While the text is being read from a program's own copy, the program's messages are pumped (so
    // it can act on the keys that were just sent to it); keys that arrive meanwhile are its own.
    bool _readingSource = false;
    std::wstring _textNote;                   // shown beside the answer (a shortened selection…)
    // The translate key (Ctrl+C by default): the second press is taken, the first stays the
    // application's (copy).
    ULONGLONG _lastTranslateCopyTick = 0;
    UINT _translateKeyDown = 0;  // the taken press, until its key-up
    // An AI action key (ai.action_key / ai.action_keys): one press, so a tapped modifier waits for
    // its key-up (a Shift that begins a chord is not a tap).
    UINT _aiActionKeyDown = 0;
    ULONGLONG _aiActionKeyDownTick = 0;  // when it went down, for the tap watch
    std::string _aiActionKeyId;  // the action it belongs to (empty: the menu)
    bool _aiActionKeyMoved = false;  // another key came while it was held
    ULONGLONG _lastTranslateKeySeen = 0;  // the last press seen (Test and Down are one press)
    UINT _translationKeyDown = 0;  // a Ctrl+0..9 taken, until its key-up
    std::optional<uint32_t> _modifierOverride;  // modifiers of a key taken by the hook
    std::uint32_t _configSubscriptionId = 0;
    bool _capsLockRefreshPending = false;

    cxxime_tsf::InputIndicatorController _inputIndicator;

    RECT _caretRect = {};
    std::uint64_t _caretRectTargetGeneration = 0;
    std::uint64_t _caretSampleSerial = 0;
    cxxime_tsf::CaretViewportTracker _caretViewportTracker;

    cxxime::UiChannelClient _uiChannel;
    std::mutex _uiCommandMutex;
    std::deque<cxxime::UiCommand> _uiCommands;
    std::optional<cxxime::UiCommand> _pendingInputIndicatorRefreshCommand;
    std::uint64_t _uiSessionGeneration = 0;
    std::uint64_t _uiTargetGeneration = 0;
    unsigned int _inputIndicatorRefreshRetryCount = 0;
    bool _inputIndicatorRefreshRetryActive = false;
    unsigned int _uiPresentationBatchDepth = 0;
    bool _uiPresentationPublishPending = false;

    std::chrono::steady_clock::time_point _key_event_start;
    int64_t _last_ipc_us = 0;
    int64_t _last_window_update_us = 0;
    cxxime::HostTraceSession _hostTraceSession;

    // Async trace writer (bounded queue, writer thread, batch flush)
    void _enqueue_trace(const TsfTrace& trace);
    void _enqueue_event_trace(const char* event, const char* detail, bool important = false);
    void _enqueue_ui_presentation_trace(const cxxime::UiPresentationSnapshot& snapshot);
};

#endif // CXXIME_TSF_TEXT_SERVICE_H_
