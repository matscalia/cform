// Behaviour tests for the cform walker (no TTY needed: a scripted prompter is
// injected). Mirrors the TypeScript test suite.
#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "artwork.hpp"
#include "json.hpp"
#include "schema.hpp"
#include "tree.hpp"
#include "tui.hpp"
#include "walker.hpp"

using namespace cform;

namespace {

int checks = 0;
int failures = 0;

void check(const std::string& name, bool ok) {
  checks++;
  if (ok) {
    std::cout << "ok   - " << name << "\n";
  } else {
    failures++;
    std::cout << "FAIL - " << name << "\n";
  }
}

struct TextAnswer {
  bool back = false;
  std::string value;
  bool jump = false;  // ctrl+f
};
struct Script {
  std::map<std::string, std::vector<TextAnswer>> text;
  std::map<std::string, std::vector<int>> select;
  std::map<std::string, std::vector<int>> confirm;
  std::map<std::string, std::vector<std::vector<int>>> multiselect;
  bool honor_validate = true;
};

struct LogEntry {
  std::string kind;
  std::string message;
  std::vector<std::string> options;
  std::optional<std::string> default_value;
  std::vector<std::string> suggest;
  std::optional<std::string> initial_value;
  std::optional<int> initial_index;
};

class ScriptedPrompter : public Prompter {
 public:
  explicit ScriptedPrompter(Script script) : script_(std::move(script)) {}

  const std::vector<LogEntry>& log() const { return log_; }

  Reply<std::string> text(const TextPrompt& prompt) override {
    log_.push_back({"text", prompt.message, {}, prompt.default_value,
                    prompt.suggest, prompt.initial_value, std::nullopt});
    if (!script_.honor_validate) return take_text(prompt);

    std::optional<std::string> last_error;
    for (int attempt = 0; attempt < 10; attempt++) {
      auto queue = script_.text.find(prompt.message);
      const bool from_queue =
          queue != script_.text.end() && !queue->second.empty();
      const Reply<std::string> answer = take_text(prompt);
      if (answer.back) return answer;
      if (!prompt.validate) return answer;
      if (const auto error = prompt.validate(answer.value)) {
        last_error = error;
        if (!from_queue) break;
        continue;
      }
      return answer;
    }
    throw std::runtime_error("scripted prompter: kept rejecting values for \"" +
                             prompt.message + "\": " +
                             last_error.value_or("(no answer)"));
  }

  Reply<int> select(const SelectPrompt& prompt) override {
    std::vector<std::string> labels;
    for (const Option& option : prompt.options) labels.push_back(option.label);
    log_.push_back({"select", prompt.message, labels, std::nullopt, {},
                    std::nullopt, prompt.initial_index});
    Reply<int> reply;
    auto queue = script_.select.find(prompt.message);
    if (queue != script_.select.end() && !queue->second.empty()) {
      reply.value = queue->second.front();
      queue->second.erase(queue->second.begin());
    } else {
      reply.value = prompt.initial_index.value_or(0);
    }
    if (reply.value == -2) throw TreeJump();
    if (reply.value < 0) {
      reply.value = 0;
      reply.back = true;
    }
    return reply;
  }

  Reply<std::vector<int>> multiselect(
      const MultiSelectPrompt& prompt) override {
    std::vector<std::string> labels;
    for (const Option& option : prompt.options) labels.push_back(option.label);
    log_.push_back({"multiselect", prompt.message, labels, std::nullopt, {},
                    std::nullopt, std::nullopt});
    Reply<std::vector<int>> reply;
    auto queue = script_.multiselect.find(prompt.message);
    if (queue != script_.multiselect.end() && !queue->second.empty()) {
      reply.value = queue->second.front();
      queue->second.erase(queue->second.begin());
    } else {
      reply.value = prompt.initial_indices;
    }
    if (!reply.value.empty() && reply.value.front() == -2) throw TreeJump();
    if (!reply.value.empty() && reply.value.front() == -1) {
      reply.value.clear();
      reply.back = true;
    }
    return reply;
  }

  Reply<bool> confirm(const ConfirmPrompt& prompt) override {
    log_.push_back({"confirm", prompt.message, {}, std::nullopt, {},
                    std::nullopt, std::nullopt});
    Reply<bool> reply;
    auto queue = script_.confirm.find(prompt.message);
    int answer = 2;
    if (queue != script_.confirm.end() && !queue->second.empty()) {
      answer = queue->second.front();
      queue->second.erase(queue->second.begin());
    }
    if (answer == -2) throw TreeJump();
    if (answer == -1) {
      reply.back = true;
      return reply;
    }
    if (answer == 2) reply.value = prompt.initial.value_or(false);
    else reply.value = answer == 1;
    return reply;
  }

  void note(const std::string& message,
            const std::optional<std::string>& title) override {
    log_.push_back({"note", title.value_or(""), {}, std::nullopt, {}, message,
                    std::nullopt});
  }

  void error(const std::string& message) override {
    log_.push_back({"error", message, {}, std::nullopt, {}, std::nullopt,
                    std::nullopt});
  }

 private:
  Reply<std::string> take_text(const TextPrompt& prompt) {
    Reply<std::string> reply;
    auto queue = script_.text.find(prompt.message);
    if (queue != script_.text.end() && !queue->second.empty()) {
      const TextAnswer entry = queue->second.front();
      queue->second.erase(queue->second.begin());
      if (entry.jump) throw TreeJump();
      reply.back = entry.back;
      reply.value = entry.value;
      return reply;
    }
    reply.value =
        prompt.default_value.value_or(prompt.placeholder.value_or(""));
    return reply;
  }

  Script script_;
  std::vector<LogEntry> log_;
};

Json parse(const std::string& text) {
  std::string error;
  const auto parsed = Json::parse(text, error);
  if (!parsed) throw std::runtime_error("test schema is invalid JSON: " + error);
  return *parsed;
}

struct Run {
  Json value;
  std::shared_ptr<ScriptedPrompter> prompter;
};

Run run(const Json& root, Script script) {
  auto prompter = std::make_shared<ScriptedPrompter>(std::move(script));
  Walker walker(root, *prompter);
  Outcome outcome;
  for (;;) {
    outcome = walker.value(root, Path{});
    if (outcome.kind != Outcome::Kind::Back) break;
  }
  return {outcome.kind == Outcome::Kind::Skip ? Json::null() : outcome.value,
          prompter};
}

/// Runs with an already partially filled document: objects are seeded from it
/// and every known field is still asked (prefilled).
Run run_seeded(const Json& root, Script script, const Json& document) {
  auto prompter = std::make_shared<ScriptedPrompter>(std::move(script));
  Walker walker(root, *prompter);
  Outcome outcome;
  for (;;) {
    outcome = walker.value(root, Path{}, {document, false, false, true});
    if (outcome.kind != Outcome::Kind::Back) break;
  }
  return {outcome.kind == Outcome::Kind::Skip ? Json::null() : outcome.value,
          prompter};
}

bool json_equal(const Json& a, const Json& b) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case Json::Type::Null:
      return true;
    case Json::Type::Bool:
      return a.boolean == b.boolean;
    case Json::Type::Number:
      return a.number == b.number;
    case Json::Type::String:
      return a.string == b.string;
    case Json::Type::Array:
      if (a.array.size() != b.array.size()) return false;
      for (std::size_t index = 0; index < a.array.size(); index++) {
        if (!json_equal(a.array[index], b.array[index])) return false;
      }
      return true;
    case Json::Type::Object:
      if (a.object.size() != b.object.size()) return false;
      for (const auto& [key, value] : a.object) {
        const Json* other = b.get(key);
        if (!other || !json_equal(value, *other)) return false;
      }
      return true;
  }
  return false;
}

bool same(const Json& a, const Json& b) { return json_equal(a, b); }

std::vector<std::string> messages(const std::vector<LogEntry>& log) {
  std::vector<std::string> result;
  for (const LogEntry& entry : log) result.push_back(entry.message);
  return result;
}

const LogEntry* find_prompt(const std::vector<LogEntry>& log,
                            const std::string& message) {
  for (const LogEntry& entry : log) {
    if (entry.message == message) return &entry;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------

void test_literal_alternatives() {
  const auto simple = literal_alternatives("^(alpha|beta)$");
  check("literal_alternatives: group alternation",
        simple && *simple == std::vector<std::string>{"alpha", "beta"});
  const auto bare = literal_alternatives("^#a|#b$");
  check("literal_alternatives: bare alternation",
        bare && *bare == std::vector<std::string>{"#a", "#b"});
  const auto escaped = literal_alternatives("^#x\\.before$");
  check("literal_alternatives: escaped dot",
        escaped && *escaped == std::vector<std::string>{"#x.before"});
  const auto triggers = literal_alternatives(
      "^(#stop_reason\\.before|#test\\.sensible\\.before)$");
  check("literal_alternatives: escaped trigger names",
        triggers && *triggers == std::vector<std::string>{"#stop_reason.before",
                                                          "#test.sensible.before"});
  check("literal_alternatives: nested groups",
        literal_alternatives("^(a|(b|c))$") &&
            *literal_alternatives("^(a|(b|c))$") ==
                std::vector<std::string>{"a", "b", "c"});
  for (const char* pattern : {"^a.*$", "^a+$", "\\d+", "^[ab]$", "^(a$"}) {
    check(std::string("literal_alternatives: rejects ") + pattern,
          !literal_alternatives(pattern).has_value());
  }
}

void test_json_roundtrip() {
  const std::string text = R"({"a":[1,2.5,"x"],"b":{"c":true},"d":null})";
  const Json parsed = parse(text);
  check("json: compact round-trip", parsed.to_compact() == text);
  check("json: pretty output",
        parse(R"({"a":1})").to_pretty() == "{\n  \"a\": 1\n}");
}

void test_object_prompts() {
  const Json root = parse(R"({
    "type": "object",
    "required": ["name", "retries"],
    "properties": {
      "name": {"type": "string", "minLength": 2},
      "retries": {"type": "integer", "minimum": 0, "default": 0},
      "mode": {"type": "string", "enum": ["fast", "slow"], "default": "fast"},
      "tags": {"type": "array", "items": {"type": "string"}, "minItems": 1},
      "notes": {"type": "string"},
      "env": {"type": "object", "additionalProperties": {"type": "string"}}
    }
  })");
  Script script;
  script.text["name"] = {{false, "orchestra"}};
  script.select["select mode"] = {1};
  script.confirm["set tags?"] = {1};
  script.text["tags[0]"] = {{false, "ai"}};
  script.confirm["add another item to tags?"] = {0};
  script.confirm["set notes?"] = {0};
  script.confirm["set env?"] = {1};
  script.text["key for env"] = {{false, "A"}};
  script.text["env.A"] = {{false, "1"}};
  script.confirm["add another entry to env?"] = {0};

  const Run result = run(root, script);
  const Json expected = parse(
      R"({"name":"orchestra","retries":0,"mode":"slow","tags":["ai"],"env":{"A":"1"}})");
  check("object: builds a value from every prompt kind",
        same(result.value, expected));
  check("object: declined optional fields are omitted",
        result.value.get("notes") == nullptr);
}

void test_required_non_empty() {
  const Json root = parse(R"({"type":"object","required":["name"],
    "properties":{"name":{"type":"string"},"tags":{"type":"array","items":{"type":"string"},"default":[]}}})");
  Script script;
  script.text["name"] = {{false, ""}, {false, "x"}};
  script.honor_validate = false;  // exercise the walker-level check
  const Run result = run(root, script);
  check("required: empty answer is re-asked", same(result.value, parse(R"({"name":"x","tags":[]})")));
  const LogEntry* error = nullptr;
  for (const LogEntry& entry : result.prompter->log()) {
    if (entry.kind == "error") error = &entry;
  }
  check("required: error message",
        error && error->message == "name: a value is required");
}

void test_ref_history() {
  const Json root = parse(R"({
    "type": "object",
    "required": ["m1", "m2", "m3", "other"],
    "properties": {
      "m1": {"type":"object","required":["provider"],"properties":{"provider":{"$ref":"#/$defs/provider"}}},
      "m2": {"type":"object","required":["provider"],"properties":{"provider":{"$ref":"#/$defs/provider"}}},
      "m3": {"type":"object","required":["provider"],"properties":{"provider":{"$ref":"#/$defs/provider"}}},
      "other": {"type":"string"}
    },
    "$defs": {"provider": {"type":"string","minLength":1}}
  })");
  Script script;
  script.text["m1.provider"] = {{false, "openai"}};
  script.text["m2.provider"] = {{false, "anthropic"}};
  script.text["m3.provider"] = {{false, "openai"}};
  script.text["other"] = {{false, "x"}};
  const Run result = run(root, script);

  const LogEntry* m2 = find_prompt(result.prompter->log(), "m2.provider");
  const LogEntry* m3 = find_prompt(result.prompter->log(), "m3.provider");
  const LogEntry* other = find_prompt(result.prompter->log(), "other");
  check("ref history: second field sees the first answer",
        m2 && m2->suggest == std::vector<std::string>{"openai"} &&
            m2->default_value == "openai");
  check("ref history: most frequent value wins",
        m3 && m3->suggest == std::vector<std::string>{"openai", "anthropic"} &&
            m3->default_value == "openai");
  check("ref history: inline schemas never share",
        other && other->suggest.empty() && !other->default_value.has_value());
}

void test_ref_enum_preselect() {
  const Json root = parse(R"({
    "type": "object",
    "required": ["a","b","c"],
    "properties": {
      "a": {"$ref":"#/$defs/mode"},
      "b": {"$ref":"#/$defs/mode"},
      "c": {"$ref":"#/$defs/mode"}
    },
    "$defs": {"mode": {"type":"string","enum":["fast","slow"]}}
  })");
  Script script;
  script.select["select a"] = {1};
  script.select["select b"] = {1};
  script.select["select c"] = {1};
  const Run result = run(root, script);
  std::vector<const LogEntry*> selects;
  for (const LogEntry& entry : result.prompter->log()) {
    if (entry.kind == "select") selects.push_back(&entry);
  }
  check("ref enum: most frequent value is preselected",
        selects.size() == 3 && selects[1]->initial_index == 1 &&
            selects[2]->initial_index == 1);
}

void test_pattern_properties() {
  const Json root = parse(R"({
    "type": "object",
    "patternProperties": {
      "^(alpha|beta)$": {"type":"string","description":"a name"},
      "^count$": {"type":"integer","minimum":0}
    },
    "additionalProperties": false
  })");
  Script script;
  script.select["key for <root>"] = {0, 1, 1};
  script.text["alpha"] = {{false, "A"}};
  script.text["count"] = {{false, "4"}};
  const Run result = run(root, script);
  check("patternProperties: typed values by key",
        same(result.value, parse(R"({"alpha":"A","count":4})")));
  const LogEntry* picker = find_prompt(result.prompter->log(), "key for <root>");
  check("patternProperties: picker lists the literal alternatives",
        picker && picker->options ==
                      std::vector<std::string>{"alpha", "beta", "count", "(done)"});
}

void test_pattern_required() {
  const Json root = parse(R"({
    "type": "object",
    "required": ["must"],
    "patternProperties": {"^(must|may)$": {"type":"string"}}
  })");
  Script script;
  script.select["key for <root>"] = {1};  // (done)
  script.text["must"] = {{false, "M"}};
  const Run result = run(root, script);
  check("patternProperties: required keys are asked first",
        same(result.value, parse(R"({"must":"M"})")));
  const std::vector<std::string> order = messages(result.prompter->log());
  check("patternProperties: required key precedes the picker",
        order.size() >= 2 && order[0] == "must" && order[1] == "key for <root>");
}

void test_pattern_non_literal() {
  const Json root = parse(
      R"({"type":"object","patternProperties":{"^a.*$":{"type":"string"}},"additionalProperties":false})");
  Script script;
  const Run result = run(root, script);
  check("patternProperties: non-literal patterns are ignored",
        same(result.value, parse(R"({})")) &&
            result.prompter->log().empty());
}

void test_back_navigation() {
  const Json root = parse(
      R"({"type":"object","required":["one","two"],"properties":{"one":{"type":"string"},"two":{"type":"string"}}})");
  Script script;
  script.text["one"] = {{false, "first"}, {false, "first-edited"}};
  script.text["two"] = {{true, ""}, {false, "second"}};
  const Run result = run(root, script);
  check("back: re-asks the previous sibling with its answer prefilled",
        same(result.value, parse(R"({"one":"first-edited","two":"second"})")));
  const std::vector<LogEntry>& log = result.prompter->log();
  std::vector<std::string> texts;
  for (const LogEntry& entry : log) {
    if (entry.kind == "text") texts.push_back(entry.message);
  }
  check("back: prompt order",
        texts == std::vector<std::string>{"one", "two", "one", "two"});
  bool prefilled = false;
  for (const LogEntry& entry : log) {
    if (entry.kind == "text" && entry.initial_value == "first") prefilled = true;
  }
  check("back: re-asked prompt is prefilled", prefilled);
}

void test_group_resume() {
  const Json root = parse(R"({
    "type":"object",
    "required":["task","next"],
    "properties":{
      "task":{"type":"object","required":["description"],"properties":{
        "description":{"type":"string"},
        "classification":{"type":"string"}
      }},
      "next":{"type":"string"}
    }
  })");
  Script script;
  script.text["task.description"] = {{false, "d"}};
  script.confirm["set task.classification?"] = {0, 0};
  script.text["next"] = {{true, ""}, {false, "n"}};
  const Run result = run(root, script);
  check("resume: group keeps earlier answers",
        same(result.value, parse(R"({"task":{"description":"d"},"next":"n"})")));
  const std::vector<std::string> order = messages(result.prompter->log());
  check("resume: lands on the last question of the group",
        order == std::vector<std::string>{"task.description",
                                          "set task.classification?", "next",
                                          "set task.classification?", "next"});
}

void test_number_constraints() {
  const Json root = parse(
      R"({"type":"object","required":["retry"],"properties":{"retry":{"type":"integer","minimum":0}}})");
  {
    Script script;
    script.text["retry"] = {{false, "-1"}, {false, "2"}};
    const Run result = run(root, script);
    check("numbers: bounds come from the schema",
          same(result.value, parse(R"({"retry":2})")));
  }
  {
    Script script;
    script.text["retry"] = {{false, "-1"}};
    bool threw = false;
    try {
      run(root, script);
    } catch (const std::exception&) {
      threw = true;
    }
    check("numbers: out of range is never accepted", threw);
  }
}

void test_allof_and_refs() {
  const Json root = parse(R"({
    "$defs": {"base": {"type":"object","required":["base"],
      "properties":{"base":{"type":"string"}}}},
    "allOf": [
      {"$ref":"#/$defs/base"},
      {"type":"object","properties":{"extra":{"type":"string"}},"required":["extra"]}
    ]
  })");
  Script script;
  script.text["base"] = {{false, "b"}};
  script.text["extra"] = {{false, "e"}};
  const Run result = run(root, script);
  check("allOf: reference properties are merged",
        same(result.value, parse(R"({"base":"b","extra":"e"})")));
}

void test_validate_value() {
  const Json schema = parse(R"({
    "type": "object",
    "required": ["name", "port", "tags"],
    "properties": {
      "name": {"type": "string", "minLength": 1},
      "port": {"type": "integer", "minimum": 0},
      "tags": {"type": "array", "minItems": 1, "items": {"type": "string"}},
      "mode": {"type": "string", "enum": ["a", "b"]}
    },
    "additionalProperties": false
  })");
  check("validate_value: complete object",
        validate_value(schema,
                       parse(R"({"name":"x","port":8080,"tags":["a"]})"),
                       schema)
            .empty());
  check("validate_value: missing required",
        !validate_value(schema, parse(R"({"name":"x","port":8080})"), schema)
             .empty());
  check("validate_value: integer type",
        !validate_value(schema,
                        parse(R"({"name":"x","port":1.5,"tags":["a"]})"),
                        schema)
             .empty());
  check("validate_value: enum",
        !validate_value(
             schema,
             parse(R"({"name":"x","port":0,"tags":["a"],"mode":"c"})"), schema)
             .empty());
  check("validate_value: additionalProperties false",
        !validate_value(schema,
                        parse(R"({"name":"x","port":0,"tags":["a"],"nope":1})"),
                        schema)
             .empty());
  check("validate_value: minItems",
        !validate_value(schema, parse(R"({"name":"x","port":0,"tags":[]})"),
                        schema)
             .empty());

  const Json pattern = parse(
      R"({"type":"object","patternProperties":{"^(a|b)$":{"type":"string"}},"additionalProperties":false})");
  check("validate_value: patternProperties key",
        validate_value(pattern, parse(R"({"a":"x"})"), pattern).empty());
  check("validate_value: unknown key",
        !validate_value(pattern, parse(R"({"c":"x"})"), pattern).empty());

  const Json nullable = parse(R"({"anyOf":[{"type":"string"},{"type":"null"}]})");
  check("validate_value: nullable",
        validate_value(nullable, Json::null(), nullable).empty());
}

void test_conditionals_validation() {
  const Json schema = parse(R"({
    "type":"object",
    "properties":{"kind":{"enum":["a","b"]}},
    "if":{"properties":{"kind":{"const":"a"}},"required":["kind"]},
    "then":{"properties":{"aValue":{"type":"string"}},"required":["aValue"]},
    "else":{"properties":{"bValue":{"type":"number"}},"required":["bValue"]}
  })");
  check("if/then/else: then valid",
        validate_value(schema, parse(R"({"kind":"a","aValue":"x"})"), schema).empty());
  check("if/then/else: then missing",
        !validate_value(schema, parse(R"({"kind":"a"})"), schema).empty());
  check("if/then/else: else valid",
        validate_value(schema, parse(R"({"kind":"b","bValue":1})"), schema).empty());
  check("if/then/else: else missing",
        !validate_value(schema, parse(R"({"kind":"b"})"), schema).empty());

  const Json not_schema = parse(R"({"type":"string","not":{"enum":["x"]}})");
  check("not: rejects", !validate_value(not_schema, Json::string_value("x"), not_schema).empty());
  check("not: accepts", validate_value(not_schema, Json::string_value("y"), not_schema).empty());
  check("not: prompt validator rejects",
        validate_string(not_schema, not_schema)("x").has_value());
  check("not: prompt validator accepts",
        !validate_string(not_schema, not_schema)("y").has_value());

  const Json allof_conditionals = parse(R"({
    "type":"object",
    "properties":{
      "backend":{"enum":["pi","opencode"]},
      "pi":{"type":"object"},
      "opencode":{"type":"object"}
    },
    "allOf":[
      {"if":{"required":["backend"],"properties":{"backend":{"const":"pi"}}},
       "then":{"required":["pi"]},
       "else":{"not":{"required":["pi"]}}},
      {"if":{"required":["backend"],"properties":{"backend":{"const":"opencode"}}},
       "then":{"required":["opencode"]},
       "else":{"not":{"required":["opencode"]}}}
    ]
  })");
  check("allOf conditionals: pi backend with pi",
        validate_value(allof_conditionals,
                       parse(R"({"backend":"pi","pi":{}})"),
                       allof_conditionals)
            .empty());
  check("allOf conditionals: pi backend with opencode",
        !validate_value(allof_conditionals,
                        parse(R"({"backend":"pi","opencode":{}})"),
                        allof_conditionals)
             .empty());
  check("allOf conditionals: opencode backend with opencode",
        validate_value(allof_conditionals,
                       parse(R"({"backend":"opencode","opencode":{}})"),
                       allof_conditionals)
            .empty());
  check("allOf conditionals: pi backend without pi",
        !validate_value(allof_conditionals, parse(R"({"backend":"pi"})"),
                        allof_conditionals)
             .empty());

  const Json any_not = parse(R"({"anyOf":[{"type":"string"}],"not":{"const":"x"}})");
  check("not with anyOf: rejects",
        !validate_value(any_not, Json::string_value("x"), any_not).empty());
  check("not with anyOf: accepts",
        validate_value(any_not, Json::string_value("y"), any_not).empty());

  const Json one_of = parse(
      R"({"oneOf":[{"type":"number"},{"type":"integer"}]})");
  check("oneOf: multiple matches are rejected",
        !validate_value(one_of, Json::number_value(1, true), one_of).empty());
  check("oneOf: single match is accepted",
        validate_value(one_of, Json::number_value(1.5, false), one_of).empty());
}

void test_conditionals_prompting() {
  const Json schema = parse(R"({
    "type":"object",
    "required":["kind"],
    "properties":{"kind":{"type":"string","enum":["a","b"]}},
    "if":{"properties":{"kind":{"const":"a"}},"required":["kind"]},
    "then":{"properties":{"aValue":{"type":"string"}},"required":["aValue"]},
    "else":{"properties":{"bValue":{"type":"number"}},"required":["bValue"]}
  })");

  Script then_script;
  then_script.select["select kind"] = {0};
  then_script.text["aValue"] = {{false, "x"}};
  const Run then_result = run(schema, then_script);
  check("if/then/else prompt: then branch field is prompted",
        same(then_result.value, parse(R"({"kind":"a","aValue":"x"})")));

  Script else_script;
  else_script.select["select kind"] = {1};
  else_script.text["bValue"] = {{false, "7"}};
  const Run else_result = run(schema, else_script);
  check("if/then/else prompt: else branch field is prompted",
        same(else_result.value, parse(R"({"kind":"b","bValue":7})")));

  auto prompt_index = [](const std::vector<LogEntry>& log,
                         const std::string& message) {
    for (std::size_t index = 0; index < log.size(); index++) {
      if (log[index].message == message) return static_cast<int>(index);
    }
    return -1;
  };
  const int then_condition =
      prompt_index(then_result.prompter->log(), "select kind");
  const int then_branch = prompt_index(then_result.prompter->log(), "aValue");
  check("if/then/else prompt: then condition is asked first",
        then_condition >= 0 && then_branch > then_condition);
  const int else_condition =
      prompt_index(else_result.prompter->log(), "select kind");
  const int else_branch = prompt_index(else_result.prompter->log(), "bValue");
  check("if/then/else prompt: else condition is asked first",
        else_condition >= 0 && else_branch > else_condition);

  const Json reordered = parse(R"({
    "type":"object",
    "properties":{
      "aValue":{"type":"string"},
      "kind":{"type":"string","enum":["a","b"]}
    },
    "required":["kind"],
    "if":{"properties":{"kind":{"const":"a"}},"required":["kind"]},
    "then":{"properties":{"aValue":{"type":"string"}},"required":["aValue"]}
  })");
  Script reordered_script;
  reordered_script.select["select kind"] = {0};
  reordered_script.text["aValue"] = {{false, "x"}};
  const Run reordered_result = run(reordered, reordered_script);
  check("if/then/else prompt: condition reorders before base property",
        same(reordered_result.value, parse(R"({"kind":"a","aValue":"x"})")));
  const int reordered_condition =
      prompt_index(reordered_result.prompter->log(), "select kind");
  const int reordered_branch =
      prompt_index(reordered_result.prompter->log(), "aValue");
  check("if/then/else prompt: condition is asked before reordered branch field",
        reordered_condition >= 0 && reordered_branch > reordered_condition);

  const Json mutual = parse(R"({
    "type":"object",
    "required":["backend"],
    "properties":{
      "backend":{"enum":["pi","opencode"]},
      "pi":{"type":"object","required":["user"],
            "properties":{"user":{"type":"string"}}},
      "opencode":{"type":"object","required":["user"],
                  "properties":{"user":{"type":"string"}}}
    },
    "allOf":[
      {"if":{"required":["backend"],
             "properties":{"backend":{"const":"pi"}}},
       "then":{"required":["pi"]},
       "else":{"not":{"required":["pi"]}}},
      {"if":{"required":["backend"],
             "properties":{"backend":{"const":"opencode"}}},
       "then":{"required":["opencode"]},
       "else":{"not":{"required":["opencode"]}}}
    ]
  })");
  Script mutual_pi_script;
  mutual_pi_script.select["select backend"] = {0};
  mutual_pi_script.text["pi.user"] = {{false, "u"}};
  const Run mutual_pi = run(mutual, mutual_pi_script);
  check("mutual exclusion prompt: pi branch",
        same(mutual_pi.value, parse(R"({"backend":"pi","pi":{"user":"u"}})")));
  check("mutual exclusion prompt: opencode not offered with pi",
        prompt_index(mutual_pi.prompter->log(), "set opencode?") < 0 &&
            prompt_index(mutual_pi.prompter->log(), "opencode.user") < 0);

  Script mutual_opencode_script;
  mutual_opencode_script.select["select backend"] = {1};
  mutual_opencode_script.text["opencode.user"] = {{false, "u"}};
  const Run mutual_opencode = run(mutual, mutual_opencode_script);
  check(
      "mutual exclusion prompt: opencode branch",
      same(mutual_opencode.value,
           parse(R"({"backend":"opencode","opencode":{"user":"u"}})")));
  check("mutual exclusion prompt: pi not offered with opencode",
        prompt_index(mutual_opencode.prompter->log(), "set pi?") < 0 &&
            prompt_index(mutual_opencode.prompter->log(), "pi.user") < 0);

  const std::vector<TreeRow> rows =
      build_tree_rows(parse(R"({"kind":"a"})"), schema, {});
  std::vector<std::string> labels;
  for (const TreeRow& row : rows) labels.push_back(row.label);
  check("if/then/else tree: active branch row is listed",
        std::find(labels.begin(), labels.end(), "aValue") != labels.end());
  check("if/then/else tree: inactive branch row is hidden",
        std::find(labels.begin(), labels.end(), "bValue") == labels.end());
}

void test_document_seed() {
  const Json root = parse(R"({
    "type": "object",
    "required": ["host", "port"],
    "properties": {
      "host": {"type": "string"},
      "port": {"type": "integer"},
      "options": {"type": "object", "properties": {
        "debug": {"type": "boolean"},
        "note": {"type": "string"}
      }}
    }
  })");
  const Json document = parse(
      R"({"host":"db","port":5432,"options":{"debug":true,"legacy":"keep"},"unknown":{"x":1}})");
  Script script;
  script.text["host"] = {{false, "db"}};
  script.text["port"] = {{false, "5432"}};
  script.confirm["set options?"] = {2};        // initial yes (prefilled)
  script.confirm["set options.debug?"] = {2};  // initial yes (prefilled)
  script.confirm["options.debug"] = {2};       // initial true
  script.confirm["set options.note?"] = {0};   // not in the document
  const Run result = run_seeded(root, script, document);
  check("document: known values are kept and unknown keys preserved",
        same(result.value, document));

  const LogEntry* host = find_prompt(result.prompter->log(), "host");
  check("document: leaves start from the document value",
        host && host->initial_value == "db" && host->default_value == "db");
}

void test_document_overrides_autocomplete() {
  const Json root = parse(R"({
    "type": "object",
    "required": ["m1", "m2"],
    "properties": {
      "m1": {"$ref": "#/$defs/provider"},
      "m2": {"$ref": "#/$defs/provider"}
    },
    "$defs": {"provider": {"type": "string"}}
  })");
  const Json document = parse(R"({"m1":"openai","m2":"azure"})");
  Script script;
  script.text["m1"] = {{false, "openai"}};
  script.text["m2"] = {{false, "azure"}};
  const Run result = run_seeded(root, script, document);
  const LogEntry* m2 = find_prompt(result.prompter->log(), "m2");
  check("document: document value overrides the autocomplete default",
        m2 && m2->default_value == "azure" && m2->initial_value == "azure" &&
            m2->suggest == std::vector<std::string>{"openai"});
}

void test_ctrl_f_persists_partial_answers() {
  const Json schema = parse(R"({"type":"object","required":["a","b"],
    "properties":{"a":{"type":"string"},"b":{"type":"string"}}})");
  Json document = parse(R"({"a":"old","b":"keep"})");

  Script script;
  script.text["a"] = {{false, "new"}};
  script.text["b"] = {{false, "", true}};  // ctrl+f at the second prompt
  auto prompter = std::make_shared<ScriptedPrompter>(script);
  Walker walker(schema, *prompter);
  walker.set_progress([&](const Path& path, const Json& value) {
    set_at_path(document, path, value);
  });

  bool jumped = false;
  try {
    walker.value(schema, Path{}, {document, false, false, true});
  } catch (const TreeJump&) {
    jumped = true;
  }
  check("ctrl+f: aborts the walk", jumped);
  check("ctrl+f: answered fields are persisted",
        same(document, parse(R"({"a":"new","b":"keep"})")));

  // restarting the same node uses the persisted value as the default
  Script again;
  auto second = std::make_shared<ScriptedPrompter>(again);
  Walker restarted(schema, *second);
  restarted.value(schema, Path{}, {document, false, false, true});
  const LogEntry* prompt = find_prompt(second->log(), "a");
  check("ctrl+f: restart ghosts the persisted value",
        prompt && prompt->initial_value == "new" &&
            prompt->default_value == "new");
}

void test_inferred_editing() {
  {
    // opaque object: members are prompted with types inferred from the document
    const Json schema = parse(R"({"type":"object"})");
    const Json document = parse(R"({"a":1,"b":"x","nest":{"c":true}})");
    Script script;
    script.confirm["add a member to <root>?"] = {0};
    const Run result = run_seeded(schema, script, document);
    check("inferred: opaque object is prompted member by member",
          same(result.value, document));
    bool raw_json = false;
    for (const auto& entry : result.prompter->log()) {
      if (entry.message.find("(json)") != std::string::npos) raw_json = true;
    }
    check("inferred: no raw JSON prompt", !raw_json);
    const LogEntry* number = find_prompt(result.prompter->log(), "a");
    check("inferred: number member keeps its type",
          number && number->initial_value == "1");
    const LogEntry* nested = find_prompt(result.prompter->log(), "nest.c");
    check("inferred: nested objects are prompted too", nested != nullptr);
  }
  {
    // loose record (additionalProperties: {}) is inferred, not raw JSON
    const Json schema = parse(R"({"type":"object","additionalProperties":{}})");
    const Json document = parse(R"({"weight":2.5,"label":"x"})");
    Script script;
    script.confirm["add a member to <root>?"] = {0};
    const Run result = run_seeded(schema, script, document);
    check("inferred: loose record keeps its members",
          same(result.value, document));
  }
  {
    // schema-less scalar: typed prompt, not raw JSON
    const Json schema = parse(R"({"type":"object","required":["any"],"properties":{"any":{}}})");
    const Json document = parse(R"({"any":7})");
    Script script;
    const Run result = run_seeded(schema, script, document);
    check("inferred: unknown scalar uses a typed prompt",
          same(result.value, document));
    const LogEntry* prompt = find_prompt(result.prompter->log(), "any");
    check("inferred: scalar prompt is not JSON",
          prompt && prompt->message.find("(json)") == std::string::npos);
  }
  {
    // absent unknown value: type picker, then the matching prompt
    const Json schema = parse(R"({"type":"object","required":["any"],"properties":{"any":{}}})");
    Script script;
    script.select["type of any"] = {1};  // number
    script.text["any"] = {{false, "7"}};
    const Run result = run(schema, script);
    check("inferred: absent value asks for a type",
          same(result.value, parse(R"({"any":7})")));
  }
  {
    // adding a member to an inferred object
    const Json schema = parse(R"({"type":"object"})");
    Script script;
    script.confirm["add a member to <root>?"] = {1, 0};
    script.text["key for <root>"] = {{false, "k"}};
    script.select["type of k"] = {0};  // string
    script.text["k"] = {{false, "v"}};
    const Run result = run(schema, script);
    check("inferred: members can be added with a type picker",
          same(result.value, parse(R"({"k":"v"})")));
  }
  {
    // opaque arrays: items inferred from the document
    const Json schema = parse(R"({"type":"array"})");
    const Json document = parse(R"([1,"x",true])");
    Script script;
    script.confirm["add an item to <root>?"] = {0};
    const Run result = run_seeded(schema, script, document);
    check("inferred: opaque array items keep their types",
          same(result.value, document));
  }
}

void test_tree_paths() {
  const Json document = parse(R"({"a":[{"b":1}]})");
  const Json* leaf = get_at_path(document, Path{}.with("a").with(0L).with("b"));
  check("tree paths: get", leaf && leaf->is_number() && leaf->number == 1);

  Json mutable_document = document;
  set_at_path(mutable_document, Path{}.with("a").with(1L).with("b"), Json::number_value(2, true));
  const Json* added = get_at_path(mutable_document, Path{}.with("a").with(1L).with("b"));
  check("tree paths: set creates entries", added && added->number == 2);
  check("tree paths: missing parent index is null",
        get_at_path(mutable_document, Path{}.with("a").with(0L)) != nullptr);

  check("tree paths: erase", erase_at_path(mutable_document, Path{}.with("a").with(0L).with("b")));
  check("tree paths: erased",
        get_at_path(mutable_document, Path{}.with("a").with(0L).with("b")) == nullptr);
}

void test_tree_rows() {
  const Json schema = parse(R"({
    "type": "object",
    "required": ["name", "port"],
    "properties": {
      "name": {"type": "string", "minLength": 1},
      "port": {"type": "integer", "minimum": 0},
      "nested": {"type": "object", "required": ["inner"],
                 "properties": {"inner": {"type": "string"}}},
      "tags": {"type": "array", "items": {"type": "string"}}
    },
    "additionalProperties": false
  })");
  const Json document = parse(R"({"name":"svc","port":"x","nested":{},"legacy":1})");
  const std::set<std::string> collapsed;
  const std::vector<TreeRow> rows = build_tree_rows(document, schema, collapsed);

  std::vector<std::string> labels;
  for (const TreeRow& row : rows) labels.push_back(row.label);
  check("tree: collapsed rows are the root and its children",
        labels == std::vector<std::string>{"document", "name", "port", "nested",
                                           "tags", "legacy"});
  check("tree: valid present leaf is ticked",
        rows[1].present && rows[1].valid && rows[1].error.empty());
  check("tree: wrong type is diagnosed",
        !rows[2].valid && rows[2].error.find("wrong type") != std::string::npos);
  check("tree: nested violation is diagnosed",
        !rows[3].valid && rows[3].error.find("inner") != std::string::npos);
  check("tree: optional unset field", !rows[4].present && rows[4].error.empty());
  check("tree: unknown property is diagnosed",
        !rows[5].valid && rows[5].error == "unknown property");
  check("tree: object with declared properties is expandable",
        rows[3].expandable && rows[3].folded);

  std::set<std::string> expanded;
  expanded.insert(path_label(Path{}.with("nested")));
  const std::vector<TreeRow> nested_rows =
      build_tree_rows(document, schema, expanded);
  std::vector<std::string> nested_labels;
  for (const TreeRow& row : nested_rows) nested_labels.push_back(row.label);
  check("tree: expanding shows the nested field",
        nested_labels == std::vector<std::string>{"document", "name", "port",
                                                  "nested", "inner", "tags",
                                                  "legacy"});
  const TreeRow& inner = nested_rows[4];
  check("tree: missing required nested field is diagnosed",
        !inner.present && inner.required && inner.error == "missing required field");
}

void test_tree_pattern_rows() {
  const Json schema = parse(
      R"({"type":"object","patternProperties":{"^(a|b)$":{"type":"string"}},"additionalProperties":false})");
  const Json document = parse(R"({"a":"x","c":1})");
  const std::vector<TreeRow> rows = build_tree_rows(document, schema, {});
  std::vector<std::string> labels;
  for (const TreeRow& row : rows) labels.push_back(row.label);
  check("tree: literal pattern keys are listed",
        labels == std::vector<std::string>{"document", "a", "b", "c"});
  check("tree: present pattern key is valid", rows[1].present && rows[1].valid);
  check("tree: absent pattern key is unset", !rows[2].present && rows[2].error.empty());
  check("tree: extra key is diagnosed",
        rows[3].error == "unknown property");
}

void test_artwork() {
  check("artwork: ansi-aware cell width",
        artwork_cell_width("\x1b[31mab\x1b[0m") == 2);
  check("artwork: utf-8 counts as one cell",
        artwork_cell_width("\xe2\x96\x80") == 1);

  const auto artwork = make_artwork("ab\r\nc\n", 4);
  check("artwork: parsed", artwork.has_value());
  check("artwork: resolved width", artwork->width == 4);
  check("artwork: resolved height", artwork->height == 2);
  check("artwork: padded lines",
        artwork->lines == std::vector<std::string>{"ab  ", "c   "});
  check("artwork: empty input", !make_artwork("").has_value());

  auto strip = [](const std::string& text) {
    std::string out;
    for (std::size_t index = 0; index < text.size();) {
      if (text[index] == '\x1b' && index + 1 < text.size() &&
          text[index + 1] == '[') {
        index += 2;
        while (index < text.size()) {
          const char code = text[index++];
          if (code >= '@' && code <= '~') break;
        }
        continue;
      }
      out += text[index++];
    }
    return out;
  };

  Artwork captioned;
  captioned.width = 6;
  captioned.height = 1;
  captioned.lines = {"abcdef"};
  ArtworkLayout layout;
  layout.caption = "hi";
  layout.caption_position = 1.0f;
  layout.caption_align = ArtworkAlign::Center;
  layout.caption_padding = 0;
  apply_artwork_caption(captioned, layout);
  check("artwork: caption keeps the width",
        artwork_cell_width(captioned.lines[0]) == 6);
  check("artwork: caption is centered", strip(captioned.lines[0]) == "  hi  ");

  Artwork clipped;
  clipped.width = 4;
  clipped.height = 1;
  clipped.lines = {"abcd"};
  layout.caption = "hello";
  layout.caption_align = ArtworkAlign::Left;
  apply_artwork_caption(clipped, layout);
  check("artwork: caption is clipped", strip(clipped.lines[0]) == "hell");

  Artwork multi;
  multi.width = 4;
  multi.height = 3;
  multi.lines = {"1111", "2222", "3333"};
  layout.caption = "x\ny";
  layout.caption_position = 1.0f;
  apply_artwork_caption(multi, layout);
  check("artwork: bottom caption rows",
        strip(multi.lines[0]) == "1111" && strip(multi.lines[1]) == "x   " &&
            strip(multi.lines[2]) == "y   ");

  multi.lines = {"1111", "2222", "3333"};
  layout.caption_position = 0.0f;
  apply_artwork_caption(multi, layout);
  check("artwork: top caption rows",
        strip(multi.lines[0]) == "x   " && strip(multi.lines[1]) == "y   " &&
            strip(multi.lines[2]) == "3333");

  multi.lines = {"1111", "2222", "3333"};
  layout.caption = "c";
  layout.caption_position = 0.5f;
  apply_artwork_caption(multi, layout);
  check("artwork: centered caption row",
        strip(multi.lines[0]) == "1111" && strip(multi.lines[1]) == "c   " &&
            strip(multi.lines[2]) == "3333");

  multi.lines = {"1111", "2222", "3333"};
  layout.caption_position = 0.75f;
  apply_artwork_caption(multi, layout);
  check("artwork: fractional caption row",
        strip(multi.lines[0]) == "1111" && strip(multi.lines[1]) == "2222" &&
            strip(multi.lines[2]) == "c   ");

  Artwork preserved;
  preserved.width = 4;
  preserved.height = 1;
  preserved.lines = {"\x1b[0m\x1b[38;2;1;2;3m\x1b[48;2;4;5;6mabcd"};
  layout.caption = "X";
  layout.caption_position = 0.0f;
  layout.caption_align = ArtworkAlign::Left;
  layout.caption_padding = 0;
  layout.caption_bg = -1;
  layout.caption_fg = 0xFFFFFF;
  apply_artwork_caption(preserved, layout);
  check("artwork: caption keeps the image background",
        preserved.lines[0].find("48;2;4;5;6") != std::string::npos);
  check("artwork: caption overlays the first glyph",
        strip(preserved.lines[0]) == "Xbcd");
  check("artwork: caption cell width unchanged",
        artwork_cell_width(preserved.lines[0]) == 4);

  preserved.lines = {"\x1b[0m\x1b[38;2;1;2;3m\x1b[48;2;4;5;6mabcd"};
  layout.caption_fg = -1;
  apply_artwork_caption(preserved, layout);
  check("artwork: caption keeps the image foreground",
        preserved.lines[0].find("38;2;1;2;3") != std::string::npos);

  preserved.lines = {"\x1b[0m\x1b[38;2;1;2;3m\x1b[48;2;4;5;6mabcd"};
  layout.caption_fg = 0xFFFFFF;
  layout.caption_bg = -1;
  layout.caption_bold = true;
  layout.caption_italic = true;
  apply_artwork_caption(preserved, layout);
  check("artwork: styled caption keeps the image background",
        preserved.lines[0].find("48;2;4;5;6") != std::string::npos);
  check("artwork: caption bold", 
        preserved.lines[0].find("\x1b[1m") != std::string::npos);
  check("artwork: caption italic",
        preserved.lines[0].find("\x1b[3m") != std::string::npos);
  check("artwork: caption style reset",
        preserved.lines[0].find("\x1b[22m") != std::string::npos &&
            preserved.lines[0].find("\x1b[23m") != std::string::npos);

  Artwork styled;
  styled.width = 6;
  styled.height = 1;
  styled.lines = {"abcdef"};
  layout.caption = "hi";
  layout.caption_position = 1.0f;
  layout.caption_align = ArtworkAlign::Center;
  layout.caption_padding = 0;
  layout.caption_bg = 0x000000;
  apply_artwork_caption(styled, layout);
  check("artwork: solid caption bold/italic",
        styled.lines[0].find("\x1b[1m") != std::string::npos &&
            styled.lines[0].find("\x1b[3m") != std::string::npos &&
            strip(styled.lines[0]) == "  hi  ");
}

}  // namespace

int main() {
  test_literal_alternatives();
  test_json_roundtrip();
  test_object_prompts();
  test_required_non_empty();
  test_ref_history();
  test_ref_enum_preselect();
  test_pattern_properties();
  test_pattern_required();
  test_pattern_non_literal();
  test_back_navigation();
  test_group_resume();
  test_number_constraints();
  test_allof_and_refs();
  test_validate_value();
  test_conditionals_validation();
  test_conditionals_prompting();
  test_document_seed();
  test_document_overrides_autocomplete();
  test_ctrl_f_persists_partial_answers();
  test_inferred_editing();
  test_tree_paths();
  test_tree_rows();
  test_tree_pattern_rows();
  test_artwork();

  std::cout << "\n" << (checks - failures) << "/" << checks << " passed\n";
  return failures == 0 ? 0 : 1;
}
