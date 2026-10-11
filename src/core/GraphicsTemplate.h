// Montage — editable graphics templates (Premiere's motion graphics templates and Essential Graphics panel, Final
// Cut's published parameters, Resolve's macros). A graphic is a sequence (titles, shapes, images...) whose designer
// publishes some of its clips' settings as fields: a title's text, a colour, a size, a switch, a choice, an image to
// swap. Every clip playing that sequence then sets those fields for itself (Clip::fields) and shows them, so one
// lower third serves every name in a film. Templates are kept in a library folder, each a project of its own with the
// files it uses beside it, and a table (CSV) can fill one graphic per row.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "EditOps.h"
#include "Model.h"

namespace montage {

// The field that would publish parameter `param` of the effect `effect` on clip `clip` of `s` (0: the clip's
// generator), its kind from the effect's catalogue entry: a colour, a switch (toggle), a choice, a number, or text
// for a text or font setting; nullopt when there is no such parameter.
std::optional<TemplateField> fieldFor(const Sequence& s, Id clip, Id effect, const std::string& param, const std::string& label = {});
// The field that swaps the picture clip `clip` plays (an image or a video); nullopt for a clip with none.
std::optional<TemplateField> mediaFieldFor(const Project& p, const Sequence& s, Id clip, const std::string& label = {});

// Publishes `f` on `s` (its id made from its label, unique in `s`, when empty or taken); `id` gets it. The same
// setting is published once.
edit::Result publishField(Sequence& s, TemplateField f, std::string* id = nullptr);
edit::Result unpublishField(Sequence& s, const std::string& id);
// The fields of `s` whose clip and effect are still there, in order.
std::vector<TemplateField> liveFields(const Sequence& s);

// The sequence clip `c` plays when it is a graphic with fields; nullptr otherwise.
const Sequence* graphicOf(const Project& p, const Clip& c);
// A field's value as text: the template's own setting (colours "#rrggbb", switches "1" or "0", numbers, a choice's
// index, a media id), or the clip's own value when it has one.
std::string fieldValue(const Sequence& tpl, const TemplateField& f);
std::string fieldValue(const Sequence& tpl, const Clip& graphic, const TemplateField& f);
// `in` as a value for `f`: a colour as "#rrggbb" (or "#rgb", "r,g,b" in 0-255), a switch from 1/0/true/false/yes/no/
// on/off, a number within the parameter's range, a choice by index or by name, a media id of an image or video in
// `p`; false (with a reason) when it cannot be one.
bool normalizeFieldValue(const Project& p, const Sequence& tpl, const TemplateField& f, const std::string& in, std::string& out,
                         std::string* error = nullptr);
// A number field's range, a choice field's choices (false for a field whose setting is gone).
bool fieldRange(const Sequence& tpl, const TemplateField& f, double& min, double& max, std::vector<std::string>* choices = nullptr);
// `tpl` with `values` (by field id) put in: what a clip with those values shows. Values that are not valid are left out.
Sequence withFieldValues(const Project& p, const Sequence& tpl, const std::map<std::string, std::string>& values);

// Fills graphics from a table (Premiere's data-driven graphics): the header names fields by id, else label (any case);
// the first row sets graphic `clip`, each further row a copy of it straight after the one before, on its track where
// that is free, else on the track above (added if needed). `made` gets the clips (`clip` first), `unmatched` the
// columns that name no field. Rows with a value that cannot be one fail the whole fill.
edit::Result fillGraphicsFromTable(Project& p, Sequence& s, Id clip, const std::string& csv, std::vector<Id>* made = nullptr,
                                   std::vector<std::string>* unmatched = nullptr);

// ---- The library -----------------------------------------------------------------------------------------------------

struct GraphicsTemplateInfo {
    std::string name;
    std::string path;  // its project file
    std::vector<std::string> fields;  // labels
    double seconds = 0;
    int width = 0, height = 0;
};

// Where templates are kept: $MONTAGE_TEMPLATES_DIR, else "Graphics Templates" in the app's data folder.
std::string graphicsTemplatesDirectory();
// Saves sequence `sequence` of `p` (with the sequences it nests, and the files its clips play copied beside it) as
// template `name` in `folder`: <folder>/<name>/<name>.montage, replacing one of that name (only a template: another
// folder of that name is refused). It is written beside it and swapped in, so a failure leaves the old one. `path` gets
// the file.
bool saveGraphicsTemplate(const Project& p, Id sequence, const std::string& name, const std::string& folder, std::string* path = nullptr,
                          std::string* error = nullptr);
// The folder (and file) name template `name` is kept under: characters files cannot have become "_", leading dots
// and trailing dots and spaces go ("Graphic" if nothing is left).
std::string graphicsTemplateFolderName(const std::string& name);
// The templates in `folder`, by name.
std::vector<GraphicsTemplateInfo> listGraphicsTemplates(const std::string& folder);
// Brings the template at `path` into `p` (its sequences and media, with new ids) and places a clip of it at `at` on
// video track `track` (-1: a new one on top), its own length. The result's first created clip is the graphic.
edit::Result insertGraphicsTemplate(Project& p, Sequence& s, const std::string& path, FrameTime at, int track = -1);

}  // namespace montage
