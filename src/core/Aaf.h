// Montage — writing AAF (Advanced Authoring Format) files: objects of the
// AAF object model stored in a compound file the way the AAF SDK and pyaaf2
// lay them out. Each object is a storage with its class ID and a "properties"
// stream; strong references are storages beneath it (with an index stream
// for vectors and sets), weak references name an object in a set by its
// key, through the file's "referenced properties" table. The MetaDictionary
// (the model's class and type definitions) is copied in from pyaaf2's.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Cfb.h"

namespace montage {

// A 16-byte AUID, as written: parsed from "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
// with the first three groups little-endian (a GUID's layout).
std::string aafAuid(const std::string& text);
// A new SMPTE UMID for a mob (32 bytes; the material number random).
std::string aafNewMobId();

// The well-known places weak references point into (indices of the file's referenced-property table).
enum class AafRefTable : uint16_t {
    ClassDefinitions = 0,
    TypeDefinitions = 1,
    DataDefinitions = 2,
    ContainerDefinitions = 3,
    OperationDefinitions = 4,
    ParameterDefinitions = 5,
    InterpolationDefinitions = 6,
};

class AafObject {
public:
    explicit AafObject(const std::string& classAuidText);

    AafObject& bytes(uint16_t pid, std::string value);
    AafObject& text(uint16_t pid, const std::string& utf8);  // UTF-16, null-terminated
    AafObject& i64(uint16_t pid, int64_t v);
    AafObject& u32(uint16_t pid, uint32_t v);
    AafObject& u16(uint16_t pid, uint16_t v);
    AafObject& u8(uint16_t pid, uint8_t v);
    AafObject& rational(uint16_t pid, int32_t num, int32_t den);
    AafObject& now(uint16_t pid);  // a TimeStamp
    // An Indirect value holding a Rational (parameter values).
    AafObject& indirectRational(uint16_t pid, int32_t num, int32_t den);

    AafObject& strong(uint16_t pid, const std::string& name, std::unique_ptr<AafObject> child);
    AafObject& add(uint16_t pid, const std::string& name, std::unique_ptr<AafObject> child);  // strong reference vector
    // Strong reference set, each element keyed by its property `keyPid` (16 or 32 bytes).
    AafObject& addToSet(uint16_t pid, const std::string& name, uint16_t keyPid, const std::string& key, std::unique_ptr<AafObject> child);
    // A weak reference to the object with `key` in the set `table` names (definitions are keyed by 0x1b01, types by 0x0005).
    AafObject& weak(uint16_t pid, AafRefTable table, uint16_t keyPid, const std::string& key);
    AafObject& weakSet(uint16_t pid, const std::string& name, AafRefTable table, uint16_t keyPid, const std::vector<std::string>& keys);

    CfbEntry storage(const std::string& name) const;

private:
    struct Child {
        std::string name;  // storage name
        std::unique_ptr<AafObject> object;
    };
    struct Prop {
        uint16_t pid = 0;
        uint8_t format = 0x82;
        std::string data;
        // Collections: the storages and the index stream.
        std::string indexName;
        std::vector<Child> children;
        std::vector<std::string> keys;  // sets: one per child; weak sets: the referenced keys
        uint16_t keyPid = 0;
        uint16_t table = 0;
    };
    Prop& prop(uint16_t pid, uint8_t format);

    std::string classId_;
    std::vector<Prop> props_;
};

// Writes the file: the MetaDictionary, `header` (a Header object) and the referenced-property table.
bool writeAafFile(const std::string& path, const AafObject& header, std::string* error = nullptr);

}  // namespace montage
