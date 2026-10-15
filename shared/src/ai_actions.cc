// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/ai_actions.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <json.hpp>

#include <cxxime/data_path.h>

namespace cxxime {

namespace {

AiAction action_from_json(const nlohmann::json& row) {
    AiAction action;
    if (row.contains("id") && row["id"].is_string()) action.id = row["id"].get<std::string>();
    if (row.contains("label") && row["label"].is_string()) {
        action.label = row["label"].get<std::string>();
    }
    if (row.contains("label_en") && row["label_en"].is_string()) {
        action.label_en = row["label_en"].get<std::string>();
    }
    if (row.contains("prompt") && row["prompt"].is_string()) {
        action.prompt = row["prompt"].get<std::string>();
    }
    if (row.contains("insert") && row["insert"].is_string()) {
        action.replace = row["insert"].get<std::string>() != "preview";
    }
    if (row.contains("max_input_chars") && row["max_input_chars"].is_number_integer()) {
        action.max_input_chars = (std::max)(0, row["max_input_chars"].get<int>());
    }
    if (row.contains("timeout_ms") && row["timeout_ms"].is_number_integer()) {
        action.timeout_ms = (std::max)(1000, row["timeout_ms"].get<int>());
    }
    return action;
}

std::vector<AiAction> load() {
    std::vector<AiAction> actions;
    std::ifstream file(std::filesystem::path(data_path("ai_actions.json")));
    if (!file) return actions;
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(text);
    } catch (const std::exception&) {
        return actions;  // a broken file leaves the feature off rather than half on
    }
    if (!parsed.is_object() || !parsed.contains("actions") || !parsed["actions"].is_array()) {
        return actions;
    }
    for (const auto& row : parsed["actions"]) {
        if (!row.is_object()) continue;
        AiAction action = action_from_json(row);
        // An action without an id, a prompt or a name has nothing to run or show.
        if (action.id.empty() || action.prompt.empty() ||
            (action.label.empty() && action.label_en.empty())) {
            continue;
        }
        actions.push_back(std::move(action));
    }
    return actions;
}

} // namespace

const std::vector<AiAction>& ai_actions() {
    static const std::vector<AiAction> actions = load();
    return actions;
}

const AiAction* find_ai_action(const std::string& id) {
    const std::vector<AiAction>& actions = ai_actions();
    const auto found = std::find_if(actions.begin(), actions.end(),
                                    [&](const AiAction& action) { return action.id == id; });
    return found == actions.end() ? nullptr : &*found;
}

const std::string& ai_action_label(const AiAction& action, const std::string& ui_language) {
    const bool english = ui_language.compare(0, 2, "en") == 0;
    if (english && !action.label_en.empty()) return action.label_en;
    return action.label;
}

} // namespace cxxime
