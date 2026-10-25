#include "SdhCaptions.h"

#include <QString>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace montage {

const char* sdhStyleName(SdhStyle s) {
    switch (s) {
        case kSdhCapitals: return "capitals";
        case kSdhColours: return "colours";
        default: return "brackets";
    }
}

bool parseSdhStyle(const std::string& name, SdhStyle& out) {
    const QString n = QString::fromStdString(name).trimmed().toLower();
    if (n == QStringLiteral("brackets") || n == QStringLiteral("sentence")) out = kSdhBrackets;
    else if (n == QStringLiteral("capitals") || n == QStringLiteral("caps")) out = kSdhCapitals;
    else if (n == QStringLiteral("colours") || n == QStringLiteral("colors")) out = kSdhColours;
    else return false;
    return true;
}

namespace {

QString sentenceCase(const QString& s) {
    QString t = s.simplified();
    if (!t.isEmpty()) t[0] = t[0].toUpper();
    return t;
}

int wordCount(const QString& s) { return int(s.split(QLatin1Char(' '), Qt::SkipEmptyParts).size()); }

// Puts `prefix` and `suffix` round a caption's text (keeping its lines when the first still fits, else wrapping it
// again) with its word times kept in step: the added words at its start and end.
void wrapWith(Caption& c, const QString& prefix, const QString& suffix, int lineChars, int maxLines) {
    const int oldWords = wordCount(QString::fromStdString(c.text).simplified());
    QStringList lines = QString::fromStdString(c.text).split(QLatin1Char('\n'));
    if (lines.isEmpty()) return;
    lines.front() = prefix + lines.front();
    lines.back() += suffix;
    bool fits = true;
    for (const QString& l : lines) fits = fits && l.size() <= lineChars;
    c.text = fits ? lines.join(QLatin1Char('\n')).toStdString() : wrapCaptionText(lines.join(QLatin1Char(' ')).toStdString(), lineChars, maxLines);
    if (c.wordTimes.size() == size_t(oldWords)) {
        std::vector<double> times(size_t(wordCount(prefix)), 0.0);
        times.insert(times.end(), c.wordTimes.begin(), c.wordTimes.end());
        times.insert(times.end(), size_t(wordCount(suffix)), 0.999);
        c.wordTimes = std::move(times);
    } else {
        c.wordTimes.clear();
    }
}

using Span = std::pair<FrameTime, FrameTime>;

// Where a caption's block sits, as fractions of the frame (as the viewer draws it).
struct Box {
    double x0, y0, x1, y1;
};
Box captionBox(const Caption& c, const SdhOptions& o) {
    const QStringList lines = QString::fromStdString(c.text).split(QLatin1Char('\n'));
    qsizetype longest = 0;
    for (const QString& l : lines) longest = std::max(longest, l.size());
    const double size = std::clamp(o.look.size, 0.01, 0.5);
    const double h = double(lines.size()) * size * 1.32 + size * 0.2;     // lines of 1.1 × the font's height, and the box
    const double w = (double(longest) * 0.5 + 0.6) * size / std::max(0.1, o.aspect);  // half an em a character
    const double bottom = c.vertical == kCaptionTop ? 1 - std::clamp(o.look.position, 0.05, 1.0) + h
                          : c.vertical == kCaptionMiddle ? 0.5 + h / 2
                                                         : std::clamp(o.look.position, 0.05, 1.0);
    const double left = c.align == kCaptionLeft ? 0.05 : c.align == kCaptionRight ? 0.95 - w : 0.5 - w / 2;
    return {left, bottom - h, left + w, bottom};
}

}  // namespace

std::string sdhSoundText(const std::string& label, SdhStyle style) {
    const QString s = sentenceCase(QString::fromStdString(label));
    if (s.isEmpty()) return {};
    switch (style) {
        case kSdhCapitals: return ("[" + s.toUpper() + "]").toStdString();
        case kSdhColours: return s.toUpper().toStdString();
        default: return ("[" + s + "]").toStdString();
    }
}

std::string sdhSpeakerPrefix(const std::string& name, SdhStyle style) {
    const QString n = QString::fromStdString(name).simplified();
    if (n.isEmpty()) return {};
    switch (style) {
        case kSdhCapitals: return (n.toUpper() + ": ").toStdString();
        case kSdhColours: return {};
        default: return ("[" + n + "] ").toStdString();
    }
}

const std::vector<std::string>& sdhSpeakerColours() {
    static const std::vector<std::string> colours = {"", "#ffff00", "#00ffff", "#00ff00"};
    return colours;
}

std::vector<size_t> sdhSpeakerChanges(const std::vector<SdhLine>& lines) {
    std::vector<size_t> out;
    for (size_t i = 0; i < lines.size(); ++i)
        if (!lines[i].speaker.empty() && (i == 0 || lines[i - 1].speaker != lines[i].speaker)) out.push_back(i);
    return out;
}

std::vector<Caption> sdhCaptions(const std::vector<SdhLine>& dialogue, const std::vector<SdhSound>& soundsIn,
                                 const std::vector<SdhText>& text, Rational fps, const SdhOptions& o, SdhSummary* summary) {
    const double f = fps.valid() && fps.toDouble() > 0 ? fps.toDouble() : 30.0;
    SdhSummary sum;
    const int lineChars = std::max(8, o.lineChars), maxLines = std::max(1, o.maxLines);
    std::vector<Caption> out;

    // The dialogue: songs, then speakers.
    std::map<std::string, std::string> colourOf;  // the colours style: each speaker's colour; "-" = named instead
    if (o.speakerLabels && o.style == kSdhColours) {
        std::map<std::string, std::pair<int, size_t>> lines;  // lines said, first line
        for (size_t i = 0; i < dialogue.size(); ++i) {
            const std::string& who = dialogue[i].speaker;
            if (who.empty()) continue;
            auto [it, fresh] = lines.try_emplace(who, 0, i);
            ++it->second.first;
        }
        std::vector<std::pair<std::string, std::pair<int, size_t>>> order(lines.begin(), lines.end());
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
            return a.second.first != b.second.first ? a.second.first > b.second.first : a.second.second < b.second.second;
        });
        const auto& colours = sdhSpeakerColours();
        for (size_t k = 0; k < order.size(); ++k) colourOf[order[k].first] = k < colours.size() ? colours[k] : "-";
    }
    const std::vector<size_t> changes = sdhSpeakerChanges(dialogue);
    for (size_t i = 0; i < dialogue.size(); ++i) {
        const SdhLine& line = dialogue[i];
        Caption c = line.caption;
        if (o.lyrics && line.sung) {
            wrapWith(c, QStringLiteral("♪ "), QStringLiteral(" ♪"), lineChars + 2, maxLines);
            c.italic = true;
            ++sum.lyrics;
        }
        if (o.speakerLabels && !line.speaker.empty()) {
            const bool change = std::binary_search(changes.begin(), changes.end(), i);
            const bool unseen = line.faces != 1;  // no face, several, or not looked at
            bool counted = false;
            if (o.style == kSdhColours) {
                const std::string& colour = colourOf[line.speaker];
                if (colour == "-") {
                    if (change && unseen) wrapWith(c, QString::fromStdString(sdhSpeakerPrefix(line.speaker, kSdhCapitals)), {}, lineChars, maxLines), counted = true;
                } else if (!colour.empty()) {
                    c.colour = colour;
                    counted = true;
                }
            } else if (change && unseen) {
                wrapWith(c, QString::fromStdString(sdhSpeakerPrefix(line.speaker, o.style)), {}, lineChars, maxLines);
                counted = true;
            }
            sum.speakers += counted;
        }
        out.push_back(std::move(c));
    }
    normalizeCaptions(out);

    // The sounds: the same one again soon after is one cue.
    std::vector<SdhSound> sounds;
    for (const SdhSound& s : soundsIn)
        if (s.end > s.start && !QString::fromStdString(s.label).trimmed().isEmpty()) sounds.push_back(s);
    std::stable_sort(sounds.begin(), sounds.end(), [](const SdhSound& a, const SdhSound& b) { return a.start < b.start; });
    std::vector<SdhSound> merged;
    for (const SdhSound& s : sounds) {
        // The last cue of the same sound, if this follows it closely.
        SdhSound* same = nullptr;
        for (auto it = merged.rbegin(); it != merged.rend(); ++it)
            if (QString::fromStdString(it->label).simplified().compare(QString::fromStdString(s.label).simplified(), Qt::CaseInsensitive) == 0) {
                if (s.start - it->end <= o.mergeSeconds) same = &*it;
                break;
            }
        if (same) {
            same->end = std::max(same->end, s.end);
            same->score = std::max(same->score, s.score);
        } else {
            merged.push_back(s);
        }
    }
    // Kept clear of the dialogue, and of each other.
    const FrameTime gap = std::max(0, o.gapFrames);
    const FrameTime minLen = std::max<FrameTime>(1, FrameTime(std::ceil(o.minSeconds * f - 1e-9)));
    const FrameTime maxLen = std::max(minLen, FrameTime(std::floor(o.maxSeconds * f + 1e-9)));
    std::vector<Span> blocked;
    for (const Caption& c : out) blocked.push_back({c.start - gap, c.end + gap});
    auto sortBlocked = [&] { std::sort(blocked.begin(), blocked.end()); };
    sortBlocked();
    std::vector<Caption> cues;
    for (const SdhSound& s : merged) {
        const FrameTime a = std::max<FrameTime>(0, FrameTime(std::floor(s.start * f + 1e-9)));
        const FrameTime b = std::max(a + 1, FrameTime(std::ceil(s.end * f - 1e-9)));
        FrameTime at = a;
        while (at < b) {
            // Past whatever is on screen at `at`.
            bool moved = false;
            for (const Span& sp : blocked)
                if (sp.first <= at && at < sp.second) at = sp.second, moved = true;
            if (moved) continue;
            FrameTime next = std::numeric_limits<FrameTime>::max();
            FrameTime nextEnd = next;
            for (const Span& sp : blocked)
                if (sp.first > at && sp.first < next) next = sp.first, nextEnd = sp.second;
            const FrameTime end = std::min({std::max(b, at + minLen), at + maxLen, next});
            if (end - at >= minLen) {
                Caption c;
                c.start = at;
                c.end = end;
                c.text = wrapCaptionText(sdhSoundText(s.label, o.style), lineChars, maxLines);
                cues.push_back(std::move(c));
                blocked.push_back({at - gap, end + gap});
                sortBlocked();
                break;
            }
            at = nextEnd;  // no room before the next caption: after it, if the sound goes on
        }
    }
    sum.sounds = int(cues.size());
    out.insert(out.end(), cues.begin(), cues.end());
    normalizeCaptions(out);

    sum.raised = sdhRaiseOverText(out, text, fps, o);
    if (summary) *summary = sum;
    return out;
}

int sdhRaiseOverText(std::vector<Caption>& captions, const std::vector<SdhText>& text, Rational fps, const SdhOptions& o) {
    const double f = fps.valid() && fps.toDouble() > 0 ? fps.toDouble() : 30.0;
    int raised = 0;
    for (Caption& c : captions) {
        if (c.vertical != kCaptionBottom) continue;
        const Box box = captionBox(c, o);
        const double from = double(c.start) / f, to = double(c.end) / f;
        const bool covers = std::any_of(text.begin(), text.end(), [&](const SdhText& t) {
            return t.start < to && from < t.end && t.x0 < box.x1 && box.x0 < t.x1 && t.y0 < box.y1 && box.y0 < t.y1;
        });
        if (covers) {
            c.vertical = kCaptionTop;
            ++raised;
        }
    }
    return raised;
}

bool sdhCaptionArea(const Caption& c, const SdhOptions& o, double& x0, double& y0, double& x1, double& y1) {
    const Box b = captionBox(c, o);
    x0 = b.x0, y0 = b.y0, x1 = b.x1, y1 = b.y1;
    return !c.text.empty();
}

std::vector<SdhLine> sdhDialogue(const Project& p, const Sequence& seq, const CaptionRules& rules) {
    std::vector<std::string> speakers;
    std::vector<Caption> captions = captionsFromTranscripts(p, seq, rules, &speakers);
    std::vector<SdhLine> out;
    for (size_t i = 0; i < captions.size(); ++i) {
        SdhLine line;
        line.caption = std::move(captions[i]);
        if (i < speakers.size()) line.speaker = speakers[i];
        out.push_back(std::move(line));
    }
    return out;
}

SoundCaption parseSoundCaption(const std::string& entry) {
    const QString e = QString::fromStdString(entry);
    const qsizetype bar = e.indexOf(QLatin1Char('|'));
    SoundCaption s;
    s.caption = sentenceCase(bar < 0 ? e : e.left(bar)).toStdString();
    const QString prompt = bar < 0 ? QString() : e.mid(bar + 1).simplified();
    s.prompt = prompt.isEmpty() ? ("the sound of " + QString::fromStdString(s.caption).toLower()).toStdString() : prompt.toStdString();
    return s;
}

std::vector<std::string> soundCaptionsFor(const Project& p) {
    return p.soundCaptions.empty() ? defaultSoundCaptions() : p.soundCaptions;
}

const std::vector<std::string>& defaultSoundCaptions() {
    // Caption-style descriptions of the sounds programmes caption most, from doors and phones to the mood of the
    // music; each with how it is described to the listener where "the sound of ..." would not say it well.
    static const std::vector<std::string> list = {
        // Doors, windows and the house.
        "Door slams|a door slamming shut", "Door opens|a door creaking open", "Door closes|a wooden door closing shut", "Door creaks|a creaking door",
        "Knocking on door|knocking on a door", "Doorbell rings|a doorbell ringing", "Door buzzer|a door buzzer", "Door locks|a key turning in a lock",
        "Keys jangle|keys jangling", "Window shatters|a window shattering", "Glass breaks|glass breaking", "Window opens|a window sliding open",
        "Curtains swish|curtains being drawn", "Drawer opens|a drawer sliding open", "Cupboard closes|a small cupboard clicking shut",
        "Floorboards creak|floorboards creaking", "Stairs creak|creaking stairs", "Clock ticking|a clock ticking", "Clock chimes|a clock chiming",
        "Cuckoo clock|a cuckoo clock", "Alarm clock rings|an alarm clock ringing", "Light switch clicks|a light switch clicking",
        "Toilet flushes|a toilet flushing", "Shower running|a shower running", "Tap running|water running from a tap",
        "Water drips|water dripping", "Kettle boils|a kettle boiling", "Kettle whistles|a kettle whistling",
        "Microwave beeps|a microwave beeping", "Dishes clatter|dishes clattering", "Cutlery clinks|cutlery clinking",
        "Glasses clink|glasses clinking in a toast", "Pouring drink|a drink being poured", "Cork pops|a cork popping",
        "Can opens|a can being opened", "Sizzling|food sizzling in a pan", "Chopping|a knife chopping on a board",
        "Blender whirs|a blender running", "Vacuum cleaner|a vacuum cleaner", "Washing machine|a washing machine running",
        "Hairdryer|a hairdryer blowing", "Smoke alarm beeps|a smoke alarm beeping", "Fire crackles|a fire crackling",
        // Phones and machines.
        "Phone rings|a telephone ringing", "Phone vibrates|a mobile phone vibrating", "Phone buzzes|a phone buzzing with a message",
        "Text message alert|a message notification sound", "Phone beeps|a phone beeping", "Dial tone|a telephone dial tone",
        "Busy tone|a telephone busy signal", "Dialling|dialling a telephone", "Phone hangs up|a phone being hung up",
        "Answering machine beeps|an answering machine beep", "Keyboard clacking|typing on a keyboard", "Mouse clicks|a computer mouse clicking",
        "Typewriter clacks|a typewriter typing", "Printer whirs|a printer printing", "Computer beeps|a computer beeping",
        "Camera shutter clicks|a camera shutter clicking", "Camera flash whines|a camera flash charging", "Radio static|radio static",
        "Radio chatter|radio chatter over a walkie-talkie", "TV playing|a television playing in the background", "Tape rewinds|a tape rewinding",
        "Record scratch|a record scratching", "Static|electrical static", "Electrical buzzing|electrical buzzing",
        "Machine whirs|a machine whirring", "Machinery clanks|heavy machinery clanking", "Engine hums|an engine humming",
        "Cash register|a cash register opening", "Coins jingle|coins jingling", "Paper rustles|paper rustling",
        "Pages turn|pages turning", "Pen scratches|a pen writing on paper", "Zipper|a zipper being pulled",
        "Bag rustles|a plastic bag rustling", "Tape rips|tape being ripped", "Scissors snip|scissors cutting",
        "Sewing machine|a sewing machine", "Lighter flicks|a lighter flicking", "Match strikes|a match being struck",
        // People without words.
        "Laughter|people laughing", "Laughs|a person laughing", "Chuckles|a person chuckling", "Giggles|giggling",
        "Audience laughs|an audience laughing", "Crying|a person crying", "Sobbing|a person sobbing", "Baby crying|a baby crying",
        "Baby babbles|a baby babbling", "Screams|a person screaming", "Shrieks|a shriek", "Shouting|people shouting",
        "Gasps|a person gasping", "Sighs|a person sighing", "Groans|a person groaning", "Grunts|a person grunting",
        "Panting|a person panting", "Breathing heavily|heavy breathing", "Coughs|a person coughing", "Sneezes|a person sneezing",
        "Sniffs|a person sniffing", "Clears throat|a person clearing their throat", "Yawns|a person yawning", "Snoring|a person snoring",
        "Whistles|a person whistling a tune", "Humming|a person humming", "Hiccups|a person hiccuping", "Gulps|a person gulping",
        "Spits|a person spitting", "Retching|a person retching", "Kiss|a kiss", "Slurping|slurping a drink", "Chewing|chewing food",
        "Crunching|crunching food", "Burps|a person burping", "Whispering|people whispering", "Indistinct chatter|indistinct chatter of many people",
        "Murmuring|a crowd murmuring", "Children playing|children playing and shouting", "Footsteps|footsteps walking",
        "Footsteps approach|footsteps approaching", "Running footsteps|footsteps running", "Footsteps on gravel|footsteps on gravel",
        "High heels clicking|high heels clicking on a hard floor", "Clapping|a person clapping", "Snaps fingers|fingers snapping",
        "Knuckles crack|knuckles cracking", "Slap|a slap", "Punch|a punch landing", "Body falls|a body falling to the ground",
        "Thud|a heavy thud", "Scuffle|a scuffle", "Struggling|people struggling",
        // Crowds and audiences.
        "Applause|an audience applauding", "Cheering|a crowd cheering", "Crowd cheers|a stadium crowd cheering",
        "Booing|a crowd booing", "Crowd chanting|a crowd chanting", "Crowd gasps|a crowd gasping", "Whooping|people whooping",
        "Crowd murmurs|a crowd murmuring", "Party chatter|people talking at a party", "Restaurant chatter|a busy restaurant",
        "Bar chatter|a noisy bar", "Classroom chatter|a noisy classroom", "Protest chants|protesters chanting",
        // Music.
        "Music playing|music playing", "Tense music|tense suspenseful music", "Suspenseful music|suspenseful music building",
        "Dramatic music|dramatic orchestral music", "Ominous music|ominous dark music", "Eerie music|eerie unsettling music",
        "Sad music|sad slow music", "Melancholy music|melancholy piano music", "Gentle music|gentle soft music",
        "Romantic music|romantic music with strings", "Upbeat music|upbeat cheerful music", "Happy music|happy bouncy music",
        "Playful music|playful light music", "Triumphant music|triumphant orchestral music", "Action music|fast action music with drums",
        "Epic music|epic orchestral music", "Mysterious music|mysterious music", "Calm music|calm relaxing music",
        "Hopeful music|hopeful uplifting music", "Funky music|funky music with bass", "Jazz music|jazz music",
        "Rock music|rock music with electric guitars", "Pop music|pop music", "Dance music|electronic dance music",
        "Hip-hop music|hip hop music", "Classical music|classical music", "Country music|country music", "Folk music|folk music",
        "Blues music|blues music", "Reggae music|reggae music", "Latin music|latin music", "Electronic music|electronic music",
        "Piano music|solo piano music", "Guitar music|acoustic guitar music", "Orchestral music|orchestral music",
        "Choir singing|a choir singing", "Organ music|church organ music", "Bagpipes|bagpipes playing", "Drumming|drums playing",
        "Drumroll|a drumroll", "Cymbal crash|a cymbal crash", "Fanfare|a trumpet fanfare", "Brass band|a brass band",
        "Marching band|a marching band", "Violin plays|a violin playing", "Harp plays|a harp playing", "Music box|a music box playing",
        "Ice cream van music|ice cream van chimes", "Music fades|music fading out", "Music stops|music stopping suddenly",
        "Radio music|music playing on a radio", "Singing|a person singing", "Whistled tune|a whistled tune",
        // Vehicles and travel.
        "Car engine starts|a car engine starting", "Car engine revs|a car engine revving", "Car passes|a car driving past",
        "Car horn honks|a car horn honking", "Tyres screech|car tyres screeching", "Car crash|a car crash",
        "Car door slams|a car door slamming", "Car alarm|a car alarm going off", "Traffic|city traffic noise",
        "Motorbike revs|a motorcycle revving", "Truck rumbles|a truck rumbling past", "Bus pulls up|a bus pulling up",
        "Air brakes hiss|air brakes hissing", "Siren wails|an emergency siren wailing", "Police siren|a police siren",
        "Ambulance siren|an ambulance siren", "Fire engine siren|a fire engine siren", "Train passes|a train passing",
        "Train horn|a train horn", "Train whistle|a steam train whistle", "Train clatters|train wheels clattering on tracks",
        "Level crossing bells|railway crossing bells", "Tram bell|a tram bell", "Underground train|an underground train arriving",
        "Plane flies overhead|an aeroplane flying overhead", "Jet engine roars|a jet engine roaring", "Plane takes off|a plane taking off",
        "Helicopter whirs|a helicopter flying", "Boat engine|a motorboat engine", "Ship's horn|a ship's horn blowing",
        "Bicycle bell|a bicycle bell ringing", "Skateboard|a skateboard rolling", "Horse hooves|horse hooves clopping",
        "Horse neighs|a horse neighing", "Reversing beeps|a vehicle reversing alarm", "Seatbelt clicks|a seatbelt clicking",
        "Indicator ticking|a car indicator ticking", "Petrol pump|fuel being pumped", "Engine sputters|an engine sputtering and dying",
        // Alarms, bells and signals.
        "Alarm blares|an alarm blaring", "Fire alarm|a fire alarm ringing", "Burglar alarm|a burglar alarm", "Bell rings|a bell ringing",
        "School bell|a school bell ringing", "Church bells|church bells ringing", "Bells toll|a bell tolling", "Gong|a gong",
        "Buzzer|a buzzer sounding", "Whistle blows|a referee's whistle blowing", "Air horn|an air horn", "Beeping|electronic beeping",
        "Heart monitor beeps|a hospital heart monitor beeping", "Flatline|a heart monitor flatlining", "Countdown beeps|countdown beeps",
        "Timer dings|a kitchen timer ding", "Lift dings|a lift arriving with a ding", "Announcement chime|a public announcement chime",
        "Tannoy announcement|an announcement over a loudspeaker", "Morse code|morse code beeping",
        // Weapons, impacts and destruction.
        "Gunshot|a gunshot", "Gunshots|several gunshots", "Gunfire|automatic gunfire", "Gun cocks|a gun being cocked",
        "Shotgun blast|a shotgun blast", "Explosion|an explosion", "Distant explosion|a distant explosion", "Bomb blast|a bomb exploding",
        "Fireworks|fireworks exploding", "Firecrackers|firecrackers popping", "Sword clashes|swords clashing",
        "Blade unsheathed|a sword being drawn", "Arrow whooshes|an arrow flying", "Whoosh|a whoosh", "Crash|a loud crash",
        "Bang|a loud bang", "Clatter|things clattering to the floor", "Rumbling|a deep rumbling", "Debris falls|debris falling",
        "Wood splinters|wood splintering", "Metal clangs|metal clanging", "Metal scrapes|metal scraping", "Chain rattles|a chain rattling",
        "Rope creaks|a rope creaking", "Building collapses|a building collapsing", "Tree falls|a tree falling",
        // Tools and work.
        "Hammering|hammering", "Drilling|a power drill", "Sawing|a saw cutting wood", "Chainsaw|a chainsaw",
        "Lawnmower|a lawnmower", "Leaf blower|a leaf blower", "Jackhammer|a jackhammer", "Construction noise|construction site noise",
        "Welding|welding sparks hissing", "Spray can|a spray can spraying", "Air hissing|air hissing", "Steam hisses|steam hissing",
        // Weather and nature.
        "Thunder|thunder rumbling", "Thunderclap|a thunderclap", "Rain|rain falling", "Heavy rain|heavy rain pouring",
        "Rain on window|rain against a window", "Wind howls|wind howling", "Wind blowing|wind blowing", "Storm|a storm raging",
        "Hail|hail falling", "Waves crash|waves crashing on a beach", "Sea|the sea lapping", "Stream babbles|a babbling stream",
        "River flows|a flowing river", "Waterfall|a waterfall", "Splash|a splash in water", "Bubbling|water bubbling",
        "Leaves rustle|leaves rustling", "Twigs snap|twigs snapping underfoot", "Footsteps in snow|footsteps in snow",
        "Crickets chirping|crickets chirping", "Cicadas|cicadas buzzing", "Frogs croaking|frogs croaking", "Birds singing|birds singing",
        "Birds chirping|birds chirping", "Owl hoots|an owl hooting", "Crows caw|crows cawing", "Seagulls|seagulls crying",
        "Pigeons coo|pigeons cooing", "Rooster crows|a rooster crowing", "Chickens cluck|chickens clucking", "Ducks quack|ducks quacking",
        "Geese honk|geese honking", "Wings flap|wings flapping", "Insect buzzes|an insect buzzing", "Bee buzzes|a bee buzzing",
        "Mosquito whines|a mosquito whining", "Fly buzzes|a fly buzzing",
        // Animals.
        "Dog barks|a dog barking", "Dogs barking|several dogs barking", "Dog growls|a dog growling", "Dog whimpers|a dog whimpering",
        "Dog howls|a dog howling", "Dog pants|a dog panting", "Cat meows|a cat meowing", "Cat purrs|a cat purring",
        "Cat hisses|a cat hissing", "Cow moos|a cow mooing", "Sheep bleat|sheep bleating", "Goat bleats|a goat bleating",
        "Pig grunts|a pig grunting", "Donkey brays|a donkey braying", "Wolf howls|a wolf howling", "Lion roars|a lion roaring",
        "Tiger growls|a tiger growling", "Elephant trumpets|an elephant trumpeting", "Monkey chatters|a monkey chattering",
        "Bear growls|a bear growling", "Snake hisses|a snake hissing", "Mouse squeaks|a mouse squeaking", "Whale song|whale song",
        // Sport and play.
        "Ball bounces|a ball bouncing", "Ball kicked|a football being kicked", "Bat hits ball|a bat hitting a ball",
        "Tennis ball|a tennis ball being hit", "Bowling pins|bowling pins falling", "Pool balls clack|pool balls colliding",
        "Skates on ice|ice skates scraping", "Starting pistol|a starting pistol", "Final whistle|a final whistle",
        "Dice roll|dice rolling", "Cards shuffle|playing cards shuffling", "Slot machine|a slot machine", "Video game sounds|video game sounds",
        "Balloon pops|a balloon popping", "Party popper|a party popper",
        // Sci-fi, horror and the uncanny.
        "Eerie whispering|eerie whispering", "Ghostly wailing|ghostly wailing", "Heartbeat|a heartbeat pounding", "Ticking|ticking",
        "Low hum|a low electronic hum", "Laser fires|a laser firing", "Spaceship hums|a spaceship humming", "Robot beeps|a robot beeping",
        "Teleport|a teleporting sound effect", "Magic chime|a magical chime", "Sparkle|a magical sparkle sound", "Static crackles|static crackling",
        "Feedback squeals|microphone feedback squealing", "Microphone thuds|a microphone being tapped", "Tinnitus ringing|a high-pitched ringing",
    };
    return list;
}

}  // namespace montage
