// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TSF_EDIT_SESSION_H_
#define CXXIME_TSF_EDIT_SESSION_H_

#include "pch.h"

#include <cstdint>
#include <optional>

class TextService;

class EditSession : public ITfEditSession {
public:
    EditSession(TextService* service, ITfContext* context);
    ~EditSession();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfEditSession
    STDMETHODIMP DoEditSession(TfEditCookie ec) override;

    enum class Action {
        INSERT_TEXT,
        INSERT_TEXT_UNDER_SELECTION,  // after the selection, not over it (insert mode "below")
        END_COMPOSITION,
        UPDATE_COMPOSITION,
        ENSURE_COMPOSITION_TEXT,
        COMMIT_AND_RESTART_COMPOSITION,
        COMMIT_COMPOSITION,
        QUERY_CARET,
        UPDATE_CANDIDATE_POSITION
    };
    void set_action(Action action, const std::wstring& text = L"");
    void set_end_composition_action(ITfComposition* composition);
    void set_composition_action(Action action,
                                const std::wstring& text,
                                size_t selection_offset,
                                size_t converted_prefix_utf16 = 0,
                                size_t focused_start_utf16 = 0,
                                size_t focused_end_utf16 = 0,
                                bool focused_converted = false,
                                const std::optional<std::wstring>& host_termination_text =
                                    std::nullopt);
    void set_position_update_from_layout_change(bool from_layout_change) {
        _positionUpdateFromLayoutChange = from_layout_change;
    }
    void set_candidate_presentation_request(uint64_t generation, uintptr_t context_identity) {
        _candidatePresentationGeneration = generation;
        _candidatePresentationContextIdentity = context_identity;
    }
    void set_composition_edit_request(uint64_t generation, uintptr_t context_identity) {
        _compositionEditGeneration = generation;
        _compositionEditContextIdentity = context_identity;
    }
    void set_commit_before_preedit(const std::wstring& text) { _commitBeforePreedit = text; }
    bool get_caret_rect(RECT& out) const {
        if (_resultValid) {
            out = _resultRect;
            return true;
        }
        return false;
    }
    bool caret_uses_viewport_fallback() const { return _resultUsesViewportFallback; }
    HRESULT action_result() const { return _actionResult; }
    bool composition_start_attempted() const { return _compositionStartAttempted; }
    HRESULT composition_start_result() const { return _compositionStartResult; }
    bool composition_returned() const { return _compositionReturned; }
    uint64_t composition_edit_generation() const { return _compositionEditGeneration; }
    uintptr_t composition_edit_context_identity() const { return _compositionEditContextIdentity; }

private:
    LONG _cRef = 1;
    TextService* _service;
    ITfContext* _context;
    ITfComposition* _expectedComposition = nullptr;
    Action _action = Action::INSERT_TEXT;
    bool _registeredWrite = false;
    std::wstring _text;
    std::wstring _commitBeforePreedit;
    size_t _selectionOffset = 0;
    bool _hasSelectionOffset = false;
    size_t _convertedPrefixUtf16 = 0;
    size_t _focusedStartUtf16 = 0;
    size_t _focusedEndUtf16 = 0;
    bool _focusedConverted = false;
    std::optional<std::wstring> _hostTerminationText;
    RECT _resultRect = {};
    bool _resultValid = false;
    bool _resultUsesViewportFallback = false;
    bool _positionUpdateFromLayoutChange = false;
    uint64_t _candidatePresentationGeneration = 0;
    uintptr_t _candidatePresentationContextIdentity = 0;
    uint64_t _compositionEditGeneration = 0;
    uintptr_t _compositionEditContextIdentity = 0;
    HRESULT _actionResult = E_PENDING;
    bool _compositionStartAttempted = false;
    HRESULT _compositionStartResult = E_PENDING;
    bool _compositionReturned = false;
};

#endif // CXXIME_TSF_EDIT_SESSION_H_
