// Montage — the Universal Category System (UCS, public domain: universalcategorysystem.com), the naming and
// categories sound effects libraries share. Its v8.2.1 list of 82 categories and 753 subcategories ships with Montage;
// each subcategory has a CatID ("DOORWood": DOORS, WOOD), which a UCS file name starts with:
// CatID[-UserCategory]_FXName_CreatorID_SourceID[_UserData], e.g. "DOORWood_Old Barn Door Creak_JD_MyLibrary.wav".
#pragma once

#include <string>
#include <vector>

namespace montage {

struct UcsCategory {
    const char* catId;        // "DOORWood"
    const char* category;     // "DOORS"
    const char* subCategory;  // "WOOD"
    const char* explanation;
    const char* synonyms;     // comma separated
};

const std::vector<UcsCategory>& ucsCategories();
// The subcategory with this CatID (letter case aside); nullptr for none.
const UcsCategory* ucsCategory(const std::string& catId);

// A UCS file name taken apart (its extension left off first).
struct UcsName {
    const UcsCategory* category = nullptr;
    std::string catId;         // as the list writes it
    std::string userCategory;  // after the CatID's "-"
    std::string fxName, creatorId, sourceId, userData;
};
// False where the name does not start with a known CatID followed by "_".
bool parseUcsFileName(const std::string& fileName, UcsName& out);

}  // namespace montage
