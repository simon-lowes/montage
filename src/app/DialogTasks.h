// Montage — what the dialogs' background tasks (DialogJobs.h, DialogTasks.cpp) keep as their results, to and from the
// JSON they are kept as in the job's folder.
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <vector>

#include "render/AutoMix.h"
#include "render/ConformCheck.h"
#include "render/Dcp.h"
#include "render/Imf.h"

namespace montage {

QJsonArray mixPlanToJson(const std::vector<ClipMix>& plan);
std::vector<ClipMix> mixPlanFromJson(const QJsonValue& v);

QJsonArray conformShotsToJson(const std::vector<ConformShot>& shots);
std::vector<ConformShot> conformShotsFromJson(const QJsonValue& v);

// What making a DCP or an IMF master left: the maker's result, what its check found ("issues", and a DCP's "advice"),
// and whether the check was stopped ("stopped").
QJsonObject dcpResultToJson(const DcpResult& r);
DcpResult dcpResultFromJson(const QJsonObject& o);
QJsonObject imfResultToJson(const ImfResult& r);
ImfResult imfResultFromJson(const QJsonObject& o);

}  // namespace montage
