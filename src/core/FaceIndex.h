// Montage — the people in footage: the faces found in each video or still
// (media/Faces.h finds them), each with its identity embedding and the
// person it was grouped with. People are found by grouping similar faces
// across the project, and can be named. Kept on the media and the project
// and saved with it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace montage {

struct Project;

struct FaceIndex {
    struct Face {
        double time = 0;                // media seconds
        float x = 0, y = 0, w = 0, h = 0;  // fractions of the frame
        float score = 0;
        float scale = 0;                // embedding = values * scale
        std::vector<int8_t> values;
        int person = 0;                 // 0: not grouped yet
        bool operator==(const Face&) const = default;
    };
    std::string model;
    double step = 0;
    std::vector<Face> faces;

    // Adds a face from a unit-length embedding (quantised to 8 bits).
    void add(double time, float x, float y, float w, float h, float score, const std::vector<float>& embedding);
    std::vector<float> embedding(size_t i) const;
    bool operator==(const FaceIndex&) const = default;
};

std::string faceIndexToJson(const FaceIndex& f);
bool faceIndexFromJson(const std::string& json, FaceIndex& out);

struct Person {
    int id = 0;
    std::string name;  // empty: "Person N"
    bool operator==(const Person&) const = default;
};

// Groups the project's faces into people: a face whose embedding is within
// `threshold` (cosine) of a group's average joins it, the clearest faces
// first. Faces already grouped stay with their person (so merges and names
// hold) and only new ones are placed; `regroup` starts again from every face,
// each group keeping the id (and name) most of its faces had before.
// Returns the number of people.
int groupPeople(Project& p, float threshold = 0.42f, bool regroup = false);
// Makes `from` the same person as `into` (who keeps their name, or takes
// `from`'s if they had none). False if either has no faces. Smart bin rules
// naming a person follow these changes.
bool mergePeople(Project& p, int from, int into);
// False if there is no such person or the name is unchanged; "" for "Person N".
bool renamePerson(Project& p, int person, const std::string& name);

std::string personName(const Project& p, int person);

struct PersonSummary {
    int id = 0;
    std::string name;
    int faces = 0, media = 0;
    // Their clearest face: where to take a picture of them from.
    uint64_t bestMedia = 0;
    double bestTime = 0;
    float x = 0, y = 0, w = 0, h = 0;
};
// Everyone found, most seen first.
std::vector<PersonSummary> peopleIn(const Project& p);

// Where a person is seen: runs of samples with their face, joined across gaps
// of up to one step (a still: 0 to 0).
struct PersonMoment {
    uint64_t media = 0;
    double start = 0, end = 0;  // media seconds
    double best = 0;            // where their face is largest
};
std::vector<PersonMoment> findPerson(const Project& p, int person);

}  // namespace montage
