#include "LensDatabase.h"

#include <QDir>
#include <QStandardPaths>
#include <cstdlib>
#include <algorithm>
#include <mutex>

#include "core/EditOps.h"
#include "core/Effects.h"

namespace montage {

namespace {

// lensfun's database at a pinned commit of lensfun/lensfun: the version 2 files of its data/db.
constexpr const char* kCommit = "43f9e001ab66c4fcdd4f400463f13b72f94b2288";

struct DbFile {
    const char* name;
    const char* sha256;
    int64_t bytes;
};

const DbFile kFiles[] = {
    {"6x6.xml", "ca13c2756b32d728675372695c072517a0e38af5eff2ecd3e17599dee7fc58b3", 29550},
    {"actioncams.xml", "79a5da0322574b95690464444c9d9ba1da0e1f827da44fd6bff120a0ec10e4b4", 17188},
    {"compact-canon.xml", "bae47e4b3a0f8852641038ae2dcca5b28ed7965e65ca795fd676f9de857c4670", 247046},
    {"compact-casio.xml", "ab35433879c4574ef8f97cd4e3d114d7ff8db4e7279da9d502fc9e911b51ee27", 6889},
    {"compact-fujifilm.xml", "58af9ebef2410c428ae2c5d01003d2247330d47d918823e63522b4d897a997d1", 67954},
    {"compact-kodak.xml", "377018ccd74f58e966b37a11e5a8689be4268153bebece9f1f2a0dbfa8234845", 2091},
    {"compact-konica-minolta.xml", "832a7473951ab625e10c48af6cf60f89b57280016d45729e2584cf9e41ad3cc7", 13349},
    {"compact-leica.xml", "8e4676679377576dc19e7900ce86fc69b0248ea1d4538aa22ee1ee09431a3be3", 9113},
    {"compact-nikon.xml", "5393bdb8223e13d21b85818e96571e97c035470b49b0452dfcedc2e1ff512d3b", 40003},
    {"compact-olympus.xml", "7637d469ad54a02ebce9492a9aa9424a2ff1ec07b0398158076f762fdc08389b", 34454},
    {"compact-panasonic.xml", "f191fc2a5d49fb50c06f4c399ee8c1a83a05f93a0efaecd352ec5c1479b8e60b", 70621},
    {"compact-pentax.xml", "596fae3edaadcf2d90ae930fc1bec42597ca0b2bd11842e686e1b915f3736390", 6689},
    {"compact-ricoh.xml", "46df96ebc367d5170cb6b7dfc3742093f0ad946264518fc5488573ba052f29d7", 18424},
    {"compact-samsung.xml", "36e3a778046d56385c387d48474cd32a89a5da94d99e640b5ce716ce747f898d", 13875},
    {"compact-sigma.xml", "0ddc265e3e580da05dcd4664d1b9c2464838d74dd02519c826520950c8f1759e", 2395},
    {"compact-sony.xml", "be146388b4f6f7bf0d73394e20386ce6ccef3559ab39f1268ba3c14aaef03207", 126229},
    {"contax.xml", "5eba12e6fd4913470afb6fe25fee47a263084b6cec830297273909872f47669a", 1696},
    {"generic.xml", "cd572d4110981546b2c4cd296c2ff16755b30063fa136d8b06ad2ca13ab4c59f", 5416},
    {"mil-canon.xml", "2de8e9467a6af6f83cc9da834d3d3b94d1aaf54519f60c68ac6bc02780a77516", 247994},
    {"mil-fujifilm.xml", "64586336153b948fe2325f6c46679149a4e3c114a70bee4bd1de88721cb4de72", 210586},
    {"mil-hasselblad.xml", "13e1fa55d36c2ed7d22964c21ce6c21d887bfe4506e840e425da1ce4157c3756", 1048},
    {"mil-leica.xml", "c691037e2e0743388473c2a0545e340462c412b7e3b57e451f4f9c89dde2388a", 11663},
    {"mil-nikon.xml", "dcae5ec2e505411a7f00b6b8dd03825e55620403b558b156be15e9ddfbeaf159", 178941},
    {"mil-olympus.xml", "989a3d88ef07a286efc2755df214792fdf71d60366932a1b369a77ad39cf2abb", 147123},
    {"mil-panasonic.xml", "eb5f01f33e65721f3900786d5970c7e220dd042aaa56968be33c009a358ff3c5", 299264},
    {"mil-pentax.xml", "675a94f52794868ad9f1d79e17b7128783c34b3db23e9eac346c5963812a16e5", 1385},
    {"mil-samsung.xml", "10130e9e125e0882a16fd3033cfed0175e9c14cb7d35e630cdb94736f27e5dc3", 13007},
    {"mil-samyang.xml", "ae3bf262f12facdc6f81b837cf6d36fdde2a930d2bfb0918d5abaa47fcfea3ab", 25990},
    {"mil-sigma.xml", "5f39b232efba5c208e5a98d8afe92cb2eee879213322d47827fcc6f0dc3a06f9", 126080},
    {"mil-sony.xml", "f9f7e5d7461ac58f76eb55dcc5b211f2e9a451402ddb4a9ecb12d1e4dc04f2b8", 274541},
    {"mil-tamron.xml", "2f0c9c4dfb9b0419ba0fac2e618bc251c2e71e068b6e09a23d7994b0ff62db0a", 106950},
    {"mil-tokina.xml", "addb27bd82b0d729644b9da1342f92401f63cc5945309ed17f38f4b7af1649e3", 1014},
    {"mil-zeiss.xml", "ef8a238904fc9ea6fdb6171863dfd70cd94814af19ecd8c31ee000ca1864589d", 19171},
    {"misc.xml", "2000a347d9dc37d908ea5b56c760de5b2b4ab475857959ae2e901c31339b01bb", 176039},
    {"om-system.xml", "a53a627e6ba79fbabb0fa7552ce2b6e56e206425d2eaf0e8ccdb6b8c40c20755", 35344},
    {"rf-leica.xml", "f93636e1980ad308d1873cc3ac078cdc77c0d529d777cd45c5c8d0e4781981e3", 6712},
    {"slr-canon.xml", "5244e6df8f2522f829c57e2073313ce31a624e9ed9605310fe43613b9d588c99", 780353},
    {"slr-hasselblad.xml", "97602c467699904bf101179ea2fa24f05f3e1879250498a6ba306cd95a3b7a6b", 642},
    {"slr-konica-minolta.xml", "5639ca94f121f609247c2c120b65830b3ef7784db6ecfa7e8eb3d29ff5046c1b", 47170},
    {"slr-leica.xml", "c5e74cb511712e99ed60a066e85c842fea58f693c5039c3819485619f5293d0e", 432},
    {"slr-nikon.xml", "5e591e8b1032203cce4c822f1da3d49524a78e7d71c594ea5a4533f2e57aacdf", 550157},
    {"slr-olympus.xml", "f7693b6779edd8ed10d7dc23113669255bfa4987cea7db0e843f7f5f2bbbe615", 116602},
    {"slr-panasonic.xml", "cd79acb49d9ce5f2a4cecc572b3e117f03da380bed2dcbfd52327192d8ce2381", 419},
    {"slr-pentax.xml", "2fba773e41a1cfb70f0c85efdab6d78944bf80ddf89bc2035ef6db39da352f29", 209552},
    {"slr-ricoh.xml", "c835798136a53535fdf4f81312ac9aad0ea3de3ef732c183d8cc4d2f432011d9", 2869},
    {"slr-samsung.xml", "dc0a29e424ff96e4316ddeb76126fa6f6c73a8836905a3e1f56f0c493826148f", 1110},
    {"slr-samyang.xml", "fb1585bea91f5f59e1fae8811dd76c92252ee5d52e3647df2aff4695048443b5", 16811},
    {"slr-schneider.xml", "de607cfe4ba6e7debc61e282ce39b72825c07601572e14af2bc9108ba67faf81", 5890},
    {"slr-sigma.xml", "f55f41c144fbb72e3c51d2dfef40e918930d37db1b4150bd02bd746406c036d2", 444640},
    {"slr-soligor.xml", "1aba0aab19bce87ea86175b6551cb5306a512eb54be1698b89991b86008f6a04", 573},
    {"slr-sony.xml", "e5ca7f532afe23ff380bf2f9e5458f687bed603355d73a254d85c4e692942fdc", 88865},
    {"slr-tamron.xml", "c62090789cebe6bbb663111c6e97beeb1111852bb735088e29cf8782df4bab45", 201992},
    {"slr-tokina.xml", "b1213eda81a613bc3976340398c6b8ce8b7dc29423476fc565d5f8e5cf612ecf", 91977},
    {"slr-ussr.xml", "1fd6acc8a8f40373379bea2899ffd96fc34b22d9b044d0474dabd4c89af44efb", 19024},
    {"slr-vivitar.xml", "c5c67e3fe38e8fa19044acadc7730047690435f66aac563f55ffe04997a88111", 2157},
    {"slr-zeiss.xml", "49aa211a41fd5fe391d26af5fa65bdf9428d60863ffff15d3916fa1c520dc101", 14957},
};

std::mutex& dbMutex() {
    static std::mutex m;
    return m;
}

std::shared_ptr<const lens::Database>& loaded() {
    static std::shared_ptr<const lens::Database> db;
    return db;
}

bool& tried() {
    static bool t = false;
    return t;
}

}  // namespace

const ModelPack& lensDatabasePack() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "lensfun-db";
        p.title = "lens profile database";
        p.directoryEnv = "MONTAGE_LENS_DB";
        p.urlEnv = "MONTAGE_LENS_DB_URL";
        for (const DbFile& f : kFiles)
            p.files.push_back({f.name,
                               std::string("https://raw.githubusercontent.com/lensfun/lensfun/") + kCommit + "/data/db/" + f.name,
                               f.sha256, f.bytes});
        return p;
    }();
    return pack;
}

std::vector<std::string> lensDatabaseFolders() {
    std::vector<std::string> out;
    if (const char* env = std::getenv("MONTAGE_LENS_DB"); env && *env) {
        out.push_back(env);
        return out;
    }
    out.push_back(lensDatabasePack().directory());
    // lensfun's own: its updates (lensfun-update-data), then the installed package's, newest format first.
    const QString home = QDir::homePath();
    QStringList roots = {home + "/.local/share/lensfun/updates", home + "/.local/share/lensfun", "/var/lib/lensfun-updates"};
    for (const QString& d : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) roots << d + "/lensfun";
    roots << "/usr/share/lensfun" << "/usr/local/share/lensfun" << "/opt/homebrew/share/lensfun" << "/opt/local/share/lensfun";
    for (const QString& r : roots)
        for (const char* v : {"/version_2", "/version_1", ""}) out.push_back((r + v).toStdString());
    return out;
}

std::shared_ptr<const lens::Database> lensDatabase() {
    std::lock_guard lock(dbMutex());
    if (tried()) return loaded();
    tried() = true;
    const char* env = std::getenv("MONTAGE_LENS_DB");
    for (const std::string& dir : lensDatabaseFolders()) {
        // (Not a download cut short.)
        if (!(env && *env) && dir == lensDatabasePack().directory() && !lensDatabasePack().installed()) continue;
        auto db = std::make_shared<lens::Database>();
        if (db->addDirectory(dir) > 0 && !db->empty()) {
            loaded() = db;
            break;
        }
    }
    return loaded();
}

void reloadLensDatabase() {
    std::lock_guard lock(dbMutex());
    tried() = false;
    loaded().reset();
}

LensChoice suggestLens(const lens::Database& db, const MediaItem& m) {
    LensChoice c;
    const auto device = m.metadata.find("device");
    if (device == m.metadata.end()) return c;
    c.camera = db.findCamera(device->second);
    if (!c.camera) return c;
    if ((c.lens = db.fixedLens(c.camera))) {
        c.focal = c.lens->minFocal;
        c.aperture = c.lens->minAperture;
    }
    return c;
}

LensChoice lensChoiceOf(const lens::Database& db, const Effect& e) {
    LensChoice c;
    if (!e.s("camera").empty()) c.camera = db.camera(e.s("camera"));
    if (!e.s("lens").empty()) c.lens = db.lens(e.s("lens"), c.camera);
    if (!c.lens && !e.s("lens").empty()) c.lens = db.lens(e.s("lens"));
    c.focal = e.p("focal", 0, 0);
    c.aperture = e.p("aperture", 0, 0);
    return c;
}

Effect lensCorrectionEffect(const LensChoice& c, Id id, const Effect* existing) {
    Effect e = existing ? *existing : makeEffect("lens_correction", id);
    if (!c.lens) return e;
    e.strings["lens"] = lens::lensName(*c.lens);
    e.strings["camera"] = c.camera ? lens::cameraName(*c.camera) : std::string();
    e.strings["profile"] = lens::lensXml(*c.lens);
    auto set = [&](const char* name, double v) {
        Param& p = e.params[name];
        p.keys.clear();
        p.value = v;
    };
    set("focal", c.focal > 0 ? c.focal : c.lens->minFocal);
    set("aperture", c.aperture > 0 ? c.aperture : c.lens->minAperture);
    set("camera_crop", c.camera && c.camera->cropFactor > 0 ? c.camera->cropFactor : 0);
    return e;
}

int applyLensCorrection(Project& p, Sequence& s, const std::vector<Id>& clips, const LensChoice& c) {
    if (!c.lens) return 0;
    int n = 0;
    for (Id id : clips) {
        const auto loc = edit::locate(s, id);
        Clip* clip = loc && loc->track.kind == TrackKind::Video ? edit::clipById(s, id) : nullptr;
        if (!clip || !clip->mediaId) continue;
        auto it = std::find_if(clip->effects.begin(), clip->effects.end(), [](const Effect& x) { return x.type == "lens_correction"; });
        if (it != clip->effects.end()) *it = lensCorrectionEffect(c, it->id, &*it);
        else clip->effects.insert(clip->effects.begin(), lensCorrectionEffect(c, p.newId()));
        ++n;
    }
    return n;
}

}  // namespace montage
