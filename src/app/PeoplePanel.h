// Montage — People: who is in the footage. Find People looks through the
// project's videos and stills for faces (media/Faces.h) and groups them into
// people across the project; each is listed with their face, can be named
// (double-click) or merged with another, and choosing one lists the moments
// they are seen, which open in the Source monitor with In and Out around them.
#pragma once

#include <QWidget>
#include <vector>

#include "core/Model.h"

class QLabel;
class QListWidget;
class QPushButton;

namespace montage {

class EditorState;
class ThumbnailCache;

// Indexes the faces in the given media (videos and stills; a subclip's media
// for a subclip) that have no face index yet, offering the model download
// first and showing progress, then groups the project's faces into people.
// False if cancelled or failed.
bool indexFacesIn(EditorState* state, const std::vector<Id>& media, QWidget* parent);

class PeoplePanel : public QWidget {
    Q_OBJECT
public:
    explicit PeoplePanel(EditorState* state, QWidget* parent = nullptr);

    // Indexes the media not looked through yet and groups everyone; false if cancelled or failed.
    bool findPeople();
    const std::vector<PersonSummary>& people() const { return people_; }
    // Lists the moments of people()[row]; returns how many.
    int selectPerson(int row);
    int currentPerson() const { return person_; }
    const std::vector<PersonMoment>& moments() const { return moments_; }
    bool rename(int person, const QString& name);
    bool merge(int from, int into);
    // A smart bin of the media the person is seen in; returns its id.
    Id makeSmartBin(int person);
    // Opens moment i in the Source monitor.
    void open(int i);
    // Saves moment i as a subclip named after the person; returns its id.
    Id makeSubclip(int i);

signals:
    void openRequested(montage::Id media, montage::FrameTime in, montage::FrameTime out, montage::FrameTime at);
    void smartBinCreated(montage::Id id);

private:
    void refresh();
    void showPeople();
    void showMoments();
    QImage faceImage(const PersonSummary& s);

    EditorState* state_;
    QLabel* status_;
    QPushButton* findBtn_;
    QListWidget* peopleList_;
    QListWidget* momentList_;
    ThumbnailCache* thumbs_;
    std::vector<PersonSummary> people_;
    std::vector<PersonMoment> moments_;
    int person_ = 0;
    bool filling_ = false;
};

}  // namespace montage
