// Montage — reading AAF (Advanced Authoring Format) compositions: Media Composer sequences, Pro Tools and Fairlight
// sessions exported as AAF, and the AAFs Premiere, Resolve and Montage write. The compound file's objects are read back
// through the AAF object model (strong references as storages, vectors and sets through their index streams, weak
// references by key) and each top-level composition becomes a sequence: its picture and sound slots become tracks,
// source clips are followed through master mobs to the file mobs and the files their network locators name (looked
// for where the AAF says, then beside it and in its "<name> Media" folder; missing files come in offline with their
// names and lengths), a sound slot plays its file's channel, fillers become gaps, transitions dissolves or crossfades
// at their cut points, audio gain becomes clip gain, a constant speed ratio clip speed, and locators markers. Effects it
// cannot carry keep their clip and are listed as warnings.
#pragma once

#include <string>

#include "Interchange.h"

namespace montage {

ImportResult importAaf(Project& p, const std::string& path, const MediaProber& probe = {});

}  // namespace montage
