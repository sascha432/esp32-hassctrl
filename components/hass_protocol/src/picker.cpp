// The entity picker of the web editor (docs/ui/web/web-dashboard-editor.md section 3) and the
// search of the screen's edit mode (docs/ui/lvgl/lvgl-dashboard.md section 7): one render_template
// each, filtered on Home Assistant's side, since get_states of a large house is more than the
// device's RAM. The search text and the entity id go in as variables, so they need no escaping in
// the template.

#include <cstring>
#include <memory>

#include "cJSON.h"
#include "hc/formats/json_writer.hpp"
#include "hc/hass_protocol/protocol.hpp"

namespace hc::hass_protocol {

namespace {

using formats::JsonWriter;

// The rows in order of the entity id (Home Assistant iterates in the order the entities were
// added), at most `n`, and no more than about `b` characters, so the answer stays below the 16 KB
// a message may have; a name counts twice (a non-ASCII letter becomes a 6 character escape).
// Every word of the search has to be in the id or the name. `more`: rows were left out.
constexpr const char *kListTemplate =
    "{%set ns=namespace(r=[],more=false,len=0)%}{%set w=(q|string|lower).split()%}"
    "{%for s in states|sort(attribute='entity_id')%}"
    "{%if (not d or s.domain==d) and not (w|reject('in',s.entity_id~' '~(s.name|lower))|list)%}"
    "{%if ns.r|length<n and ns.len<b%}"
    "{%set ns.r=ns.r+[[s.entity_id,s.name[:40],s.domain,s.attributes.get('device_class'),"
    "s.attributes.get('unit_of_measurement')]]%}"
    "{%set ns.len=ns.len+(s.entity_id|length)+2*(s.name[:40]|length)+48%}"
    "{%else%}{%set ns.more=true%}{%endif%}{%endif%}{%endfor%}"
    "{{{'more':ns.more,'rows':ns.r}|tojson}}";

// what a new tile needs of its entity; {"id": e, "missing": true} when Home Assistant has none
constexpr const char *kDetailsTemplate =
    "{%set s=states[e]%}{%if s%}{%set a=s.attributes%}"
    "{{{'id':s.entity_id,'name':s.name,'domain':s.domain,'state':s.state,'icon':a.get('icon'),"
    "'device_class':a.get('device_class'),'unit':a.get('unit_of_measurement'),"
    "'color_modes':a.get('supported_color_modes'),'temp_step':a.get('target_temp_step')}|tojson}}"
    "{%else%}{{{'id':e,'missing':true}|tojson}}{%endif%}";

constexpr int kListBudget = 10000;

template <class Body>
memory::String write(Body body)
{
    JsonWriter measure(nullptr, 0);
    body(measure);
    memory::String out(measure.needed(), '\0');
    JsonWriter writer(out.data(), out.size() + 1);
    body(writer);
    return out;
}

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

Json parse(std::string_view text)
{
    return Json(cJSON_ParseWithLength(text.data(), text.size()), cJSON_Delete);
}

} // namespace

memory::String entity_list_message(uint32_t id, std::string_view domain, std::string_view query, size_t max)
{
    return write([&](JsonWriter &w) {
        w.begin_object()
            .number("id", id)
            .string("type", "render_template")
            .string("template", kListTemplate)
            .begin_object("variables")
            .string("d", domain.data(), domain.size())
            .string("q", query.data(), query.size())
            .number("n", static_cast<int64_t>(max < kEntityListMax ? max : kEntityListMax))
            .number("b", kListBudget)
            .end_object()
            .boolean("report_errors", true)
            .end_object();
    });
}

memory::String entity_details_message(uint32_t id, std::string_view entity_id)
{
    return write([&](JsonWriter &w) {
        w.begin_object()
            .number("id", id)
            .string("type", "render_template")
            .string("template", kDetailsTemplate)
            .begin_object("variables")
            .string("e", entity_id.data(), entity_id.size())
            .end_object()
            .boolean("report_errors", true)
            .end_object();
    });
}

bool template_result_json(const cJSON *result, memory::String &out)
{
    out.clear();
    // Home Assistant parses a result that looks like JSON; older versions send the text
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> parsed(nullptr, cJSON_Delete);
    if (cJSON_IsString(result)) {
        parsed.reset(cJSON_Parse(result->valuestring));
        result = parsed.get();
    }
    if (!cJSON_IsObject(result) && !cJSON_IsArray(result)) {
        return false;
    }
    char *text = cJSON_PrintUnformatted(result);
    if (text == nullptr) {
        return false;
    }
    out = text;
    cJSON_free(text);
    return true;
}

bool parse_entity_list(std::string_view json, core::EntityList &out)
{
    out.more = false;
    out.rows.clear();
    const Json root = parse(json);
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(root.get(), "rows");
    if (!cJSON_IsArray(rows)) {
        return false;
    }
    out.more = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(), "more"));
    const cJSON *row = nullptr;
    cJSON_ArrayForEach(row, rows)
    {
        const char *id = cJSON_GetStringValue(cJSON_GetArrayItem(row, 0));
        const char *name = cJSON_GetStringValue(cJSON_GetArrayItem(row, 1));
        if (id == nullptr) {
            continue;
        }
        out.rows.push_back({memory::String(id), memory::String(name != nullptr ? name : "")});
    }
    return true;
}

bool parse_entity_details(std::string_view json, core::EntityDetails &out)
{
    const Json root = parse(json);
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root.get(), "id"));
    if (id == nullptr) {
        return false;
    }
    out.id = id;
    out.missing = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(), "missing"));
    // the rule of the web editor (web/dashboard.js): a light dims when it has a color mode besides onoff
    out.dimmer = false;
    const cJSON *mode = nullptr;
    cJSON_ArrayForEach(mode, cJSON_GetObjectItemCaseSensitive(root.get(), "color_modes"))
    {
        const char *name = cJSON_GetStringValue(mode);
        out.dimmer = out.dimmer || (name != nullptr && std::strcmp(name, "onoff") != 0);
    }
    return true;
}

} // namespace hc::hass_protocol
