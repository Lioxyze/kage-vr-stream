#include "rift_tracking.hpp"

#include <windows.h>

#include <OVR_CAPI.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kage/util/log.hpp"
#include "openvr_driver.h"

namespace kage::driver {

namespace {

struct V3 {
    double x = 0, y = 0, z = 0;
};
struct Q {
    double w = 1, x = 0, y = 0, z = 0;
};
V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 mul(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
Q qmul(Q a, Q b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
Q conj(Q q) { return {q.w, -q.x, -q.y, -q.z}; }
V3 rot(Q q, V3 v) {
    const Q r = qmul(qmul(q, {0, v.x, v.y, v.z}), conj(q));
    return {r.x, r.y, r.z};
}
Q yawQ(double a) { return {std::cos(a / 2), 0, std::sin(a / 2), 0}; }
V3 rotVec(Q q) {  // axis * angle, shortest path
    if (q.w < 0) q = {-q.w, -q.x, -q.y, -q.z};
    const double s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (s < 1e-9) return {};
    const double ang = 2 * std::atan2(s, q.w);
    return {q.x / s * ang, q.y / s * ang, q.z / s * ang};
}
double len(V3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Q slerp(Q a, Q b, double t) {
    double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < 0) {
        b = {-b.w, -b.x, -b.y, -b.z};
        d = -d;
    }
    Q r{a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    const double n = std::sqrt(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z);
    return {r.w / n, r.x / n, r.y / n, r.z / n};
}

// One Euro filter (Casiez et al. 2012): cutoff rises with speed.
struct OneEuro {
    double minCutoff = 4.0, beta = 4.0, dCutoff = 1.0;  // light: the Rift output is already smooth
    bool init = false;
    V3 x, dx;
    Q q;
    static double alpha(double cutoff, double dt) {
        const double tau = 1.0 / (2 * 3.14159265358979 * cutoff);
        return 1.0 / (1.0 + tau / dt);
    }
    void filter(V3& p, Q& o, double dt) {
        if (!init || dt <= 0) {
            init = true;
            x = p;
            q = o;
            dx = {};
            return;
        }
        const V3 rawDx = mul(sub(p, x), 1 / dt);
        const double ad = alpha(dCutoff, dt);
        dx = add(mul(dx, 1 - ad), mul(rawDx, ad));
        const double a = alpha(minCutoff + beta * len(dx), dt);
        x = add(mul(x, 1 - a), mul(p, a));
        q = slerp(q, o, a);
        p = x;
        o = q;
    }
};

struct Calibration {
    double yaw = 0;
    V3 t;
    bool valid = false;
};

// Solves A = R(yaw) * B * X: yaw from world-frame rotation deltas (X cancels),
// then translation t and the local offset x by linear least squares.
bool solveCalibration(const std::vector<std::pair<Q, V3>>& A, const std::vector<std::pair<Q, V3>>& B, Calibration& out,
                      std::string& msg) {
    const size_t n = A.size();
    if (n < 60) {
        msg = "Pas assez d'échantillons (les deux manettes doivent être suivies)";
        return false;
    }
    double sDot = 0, sCross = 0;
    int used = 0;
    for (size_t i = 0; i + 20 < n; ++i) {
        const size_t j = i + 20;  // 1 s apart
        const V3 a = rotVec(qmul(A[j].first, conj(A[i].first)));
        const V3 b = rotVec(qmul(B[j].first, conj(B[i].first)));
        if (len(a) < 0.2 || len(b) < 0.2) continue;  // need > ~11 deg of rotation
        sDot += a.x * b.x + a.z * b.z;
        sCross += a.x * b.z - a.z * b.x;
        ++used;
    }
    if (used < 10) {
        msg = "Pas assez de rotation : tourne les manettes dans tous les sens";
        return false;
    }
    // R_y(yaw) maps (x, z) -> (x cos + z sin, -x sin + z cos); maximise sum a . R b.
    const double yaw = std::atan2(sCross, sDot);
    const Q R = yawQ(yaw);
    // Unknowns u = [t (3), x (3)]: A_p - R B_p = t + (R * B_rot) x.
    double M[6][7] = {};
    for (size_t k = 0; k < n; ++k) {
        const V3 r = sub(A[k].second, rot(R, B[k].second));
        const Q RB = qmul(R, B[k].first);
        const V3 c0 = rot(RB, {1, 0, 0}), c1 = rot(RB, {0, 1, 0}), c2 = rot(RB, {0, 0, 1});
        const double rows[3][7] = {{1, 0, 0, c0.x, c1.x, c2.x, r.x}, {0, 1, 0, c0.y, c1.y, c2.y, r.y}, {0, 0, 1, c0.z, c1.z, c2.z, r.z}};
        for (const auto& row : rows)
            for (int i = 0; i < 6; ++i)
                for (int j = 0; j < 7; ++j) M[i][j] += row[i] * row[j];
    }
    for (int c = 0; c < 6; ++c) {  // Gaussian elimination with partial pivoting
        int p = c;
        for (int r = c + 1; r < 6; ++r)
            if (std::fabs(M[r][c]) > std::fabs(M[p][c])) p = r;
        if (std::fabs(M[p][c]) < 1e-9) {
            msg = "Calcul impossible : bouge davantage les manettes";
            return false;
        }
        std::swap(M[c], M[p]);
        for (int r = 0; r < 6; ++r)
            if (r != c) {
                const double f = M[r][c] / M[c][c];
                for (int j = c; j < 7; ++j) M[r][j] -= f * M[c][j];
            }
    }
    out.yaw = yaw;
    out.t = {M[0][6] / M[0][0], M[1][6] / M[1][1], M[2][6] / M[2][2]};
    const V3 x{M[3][6] / M[3][3], M[4][6] / M[4][4], M[5][6] / M[5][5]};
    double err = 0;  // residual: quality report for the user
    for (size_t k = 0; k < n; ++k) {
        const V3 pred = add(add(rot(R, B[k].second), out.t), rot(qmul(R, B[k].first), x));
        err += len(sub(A[k].second, pred));
    }
    err /= static_cast<double>(n);
    out.valid = true;
    char buf[160];
    std::snprintf(buf, sizeof buf, "Calibration réussie (écart moyen %.1f cm, rotation %.0f°)", err * 100, yaw * 57.2958);
    msg = buf;
    if (err > 0.05) msg += " - écart élevé, recommence en tenant les manettes bien serrées";
    return true;
}

// ---------------------------------------------------------------------------
class Tracker : public vr::ITrackedDeviceServerDriver {
public:
    Tracker(const char* serial, const char* role, const char* type) : serial_(serial), role_(role), type_(type) {}
    const char* serial() const { return serial_; }
    vr::EVRInitError Activate(uint32_t id) override {
        id_ = id;
        auto* p = vr::VRProperties();
        const auto c = p->TrackedDeviceToPropertyContainer(id);
        p->SetStringProperty(c, vr::Prop_ModelNumber_String, "KAGE Rift Touch tracker");
        p->SetStringProperty(c, vr::Prop_ManufacturerName_String, "KAGE");
        p->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "oculus");
        p->SetStringProperty(c, vr::Prop_SerialNumber_String, serial_);
        // Same declaration as OculusTouchLink (known to render): a plain Vive tracker.
        (void)type_;
        p->SetStringProperty(c, vr::Prop_ControllerType_String, "vive_tracker");
        p->SetStringProperty(c, vr::Prop_ResourceRoot_String, "htc");
        p->SetStringProperty(c, vr::Prop_HardwareRevision_String, "14");
        p->SetUint64Property(c, vr::Prop_HardwareRevision_Uint64, 14U);
        p->SetBoolProperty(c, vr::Prop_HasDisplayComponent_Bool, false);
        p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceOff_String, "{htc}/icons/tracker_status_off.png");
        p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceSearching_String, "{htc}/icons/tracker_status_searching.gif");
        p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceSearchingAlert_String, "{htc}/icons/tracker_status_searching_alert.gif");
        p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceReady_String, "{htc}/icons/tracker_status_ready.png");
        p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceReadyAlert_String, "{htc}/icons/tracker_status_ready_alert.png");
        p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceNotReady_String, "{htc}/icons/tracker_status_error.png");
        p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceStandby_String, "{htc}/icons/tracker_status_standby.png");
        p->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{htc}/input/vive_tracker_profile.json");
        p->SetStringProperty(c, vr::Prop_RenderModelName_String, "{htc}vr_tracker_vive_1_0");
        p->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32, vr::TrackedControllerRole_Invalid);
        p->SetInt32Property(c, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_GenericTracker);
        // Preset the SteamVR tracker role (changeable in SteamVR > Manage trackers).
        const std::string key = std::string("/devices/kage/") + serial_;
        vr::VRSettings()->SetString("trackers", key.c_str(), role_);
        return vr::VRInitError_None;
    }
    void Deactivate() override { id_ = vr::k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {}
    void* GetComponent(const char*) override { return nullptr; }
    void DebugRequest(const char*, char* r, uint32_t n) override {
        if (n) r[0] = 0;
    }
    vr::DriverPose_t GetPose() override { return pose_; }
    void update(bool valid, V3 p, Q o) {
        vr::DriverPose_t d{};
        d.poseIsValid = valid;
        d.deviceIsConnected = true;
        d.result = valid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
        d.qWorldFromDriverRotation.w = 1;
        d.qDriverFromHeadRotation.w = 1;
        d.vecPosition[0] = p.x;
        d.vecPosition[1] = p.y;
        d.vecPosition[2] = p.z;
        d.qRotation = {o.w, o.x, o.y, o.z};
        pose_ = d;
        if (id_ != vr::k_unTrackedDeviceIndexInvalid)
            vr::VRServerDriverHost()->TrackedDevicePoseUpdated(id_, pose_, sizeof pose_);
    }

private:
    const char *serial_, *role_, *type_;
    uint32_t id_ = vr::k_unTrackedDeviceIndexInvalid;
    vr::DriverPose_t pose_{};
};

Q toQ(const ovrQuatf& q) { return {q.w, q.x, q.y, q.z}; }
V3 toV(const ovrVector3f& v) { return {v.x, v.y, v.z}; }

}  // namespace

struct RiftTracking::Impl {
    ovrSession session = nullptr;
    std::filesystem::path dir;
    std::thread thread;
    std::atomic<bool> running{false};
    // Left Touch -> left foot, right Touch -> right foot, VR object 0 (3rd Touch) -> waist.
    Tracker trackers[3] = {{"KAGE-RIFT-LFOOT", "TrackerRole_LeftFoot", "vive_tracker_left_foot"},
                           {"KAGE-RIFT-RFOOT", "TrackerRole_RightFoot", "vive_tracker_right_foot"},
                           {"KAGE-RIFT-WAIST", "TrackerRole_Waist", "vive_tracker_waist"}};
    OneEuro filters[3];
    std::mutex m;
    std::atomic<double> predictS{0.04};  // Quest prediction horizon (s)
    Calibration calib;
    bool refValid = false;
    double refTime = 0;

    // Fallback reference when no KAGE stream feeds us (another streamer such as Virtual
    // Desktop owns the headset): the right-hand controller as SteamVR sees it (raw space,
    // the same space our trackers are reported in).
    static bool steamVrRightController(Q& q, V3& p) {
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
        vr::VRServerDriverHost()->GetRawTrackedDevicePoses(0, poses, vr::k_unMaxTrackedDeviceCount);
        auto* props = vr::VRProperties();
        for (uint32_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
            if (!poses[i].bPoseIsValid || !poses[i].bDeviceIsConnected) continue;
            const auto c = props->TrackedDeviceToPropertyContainer(i);
            vr::ETrackedPropertyError e;
            if (props->GetInt32Property(c, vr::Prop_DeviceClass_Int32, &e) != vr::TrackedDeviceClass_Controller) continue;
            if (props->GetInt32Property(c, vr::Prop_ControllerRoleHint_Int32, &e) != vr::TrackedControllerRole_RightHand) continue;
            const auto& m = poses[i].mDeviceToAbsoluteTracking.m;
            p = {m[0][3], m[1][3], m[2][3]};
            const double tr = m[0][0] + m[1][1] + m[2][2];
            if (tr > 0) {
                const double s = std::sqrt(tr + 1.0) * 2;
                q = {0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
            } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
                const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2;
                q = {(m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s};
            } else if (m[1][1] > m[2][2]) {
                const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2;
                q = {(m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s};
            } else {
                const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2;
                q = {(m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s};
            }
            return true;
        }
        return false;
    }
    Q refQ;
    V3 refP;

    void status(const std::string& s) {
        std::ofstream(dir / L"fullbody_status.txt", std::ios::trunc) << s;
        log::info("fullbody: {}", s);
    }
    void loadCalibration() {
        std::ifstream in(dir / L"rift_calibration.txt");
        Calibration c;
        if (in >> c.yaw >> c.t.x >> c.t.y >> c.t.z) {
            c.valid = true;
            std::lock_guard l(m);
            calib = c;
        }
    }
    void saveCalibration(const Calibration& c) {
        std::ofstream(dir / L"rift_calibration.txt", std::ios::trunc) << c.yaw << ' ' << c.t.x << ' ' << c.t.y << ' ' << c.t.z;
    }

    void loop() {
        const auto request = dir / L"calibrate.request";
        std::vector<std::pair<Q, V3>> sa, sb;
        double calibEnd = 0, calibStart = 0, lastSample = 0, last = ovr_GetTimeInSeconds(), lastCheck = 0;
        int lastTracked = -1;
        while (running) {
            const double now = ovr_GetTimeInSeconds();
            const double dt = now - last;
            last = now;
            // Predict to the same instant as the Quest poses (its display time), else the trackers
            // trail the controllers by the whole streaming latency.
            const double at = now + predictS.load();
            const ovrTrackingState ts = ovr_GetTrackingState(session, at, ovrFalse);
            ovrTrackedDeviceType objType = ovrTrackedDevice_Object0;
            ovrPoseStatef obj{};
            const bool objOk = (ovr_GetConnectedControllerTypes(session) & ovrControllerType_Object0) &&
                               OVR_SUCCESS(ovr_GetDevicePoses(session, &objType, 1, at, &obj));
            const ovrPoseStatef* src[3] = {&ts.HandPoses[0], &ts.HandPoses[1], objOk ? &obj : nullptr};
            const unsigned flags[3] = {ts.HandStatusFlags[0], ts.HandStatusFlags[1], objOk ? static_cast<unsigned>(ovrStatus_PositionTracked) : 0u};  // no status for objects
            Calibration c;
            {
                std::lock_guard l(m);
                c = calib;
            }
            const Q R = yawQ(c.yaw);
            for (int i = 0; i < 3; ++i) {
                // Shown even before calibration (raw Rift space) so the user sees them; kept
                // valid while only the orientation is tracked (short occlusions) to avoid blinking.
                const bool valid = src[i] && (flags[i] & (ovrStatus_PositionTracked | ovrStatus_OrientationTracked));
                if (!src[i]) {
                    trackers[i].update(false, {}, {});
                    continue;
                }
                V3 p = add(rot(R, toV(src[i]->ThePose.Position)), c.t);
                Q o = qmul(R, toQ(src[i]->ThePose.Orientation));
                if (valid) filters[i].filter(p, o, dt);
                trackers[i].update(valid, p, o);
            }

            // Calibration: Rift right Touch held against the Quest right controller.
            if (now - lastCheck > 0.25) {  // keep in step with the headset countdown
                lastCheck = now;
                std::error_code ec;
                if (calibEnd == 0 && std::filesystem::exists(request, ec)) {
                    std::filesystem::remove(request, ec);
                    sa.clear();
                    sb.clear();
                    calibStart = now + 3;  // countdown so the user can get in position
                    calibEnd = now + 28;
                    status("Calibration dans 3 s : colle la manette droite du Rift à celle du Quest, puis tourne-les LENTEMENT dans tous les sens pendant 25 s");
                }
            }
            if (calibEnd > 0) {
                if (now >= calibStart && now - lastSample > 0.05) {
                    lastSample = now;
                    std::lock_guard l(m);
                    Q rq = refQ;
                    V3 rp = refP;
                    bool ok = refValid && now - refTime < 0.2;
                    if (!ok) ok = steamVrRightController(rq, rp);  // e.g. Virtual Desktop session
                    if (ok && (ts.HandStatusFlags[1] & ovrStatus_PositionTracked)) {
                        sa.push_back({rq, rp});
                        sb.push_back({toQ(ts.HandPoses[1].ThePose.Orientation), toV(ts.HandPoses[1].ThePose.Position)});
                    }
                }
                if (now > calibEnd) {
                    calibEnd = 0;
                    Calibration nc;
                    std::string msg;
                    if (solveCalibration(sa, sb, nc, msg)) {
                        saveCalibration(nc);
                        std::lock_guard l(m);
                        calib = nc;
                    }
                    status(msg);
                }
            }
            // Live tracked count (Rift asleep or controller off => 0/3), shown in the GUI.
            int tracked = 0;
            for (int i = 0; i < 3; ++i) tracked += src[i] && (flags[i] & (ovrStatus_PositionTracked | ovrStatus_OrientationTracked)) ? 1 : 0;
            if (calibEnd == 0 && tracked != lastTracked) {
                lastTracked = tracked;
                status(std::string("Rift connecté : ") + std::to_string(tracked) + "/3 trackers suivis" +
                       (c.valid ? ", calibré" : " - calibration nécessaire") +
                       (tracked == 0 ? " (Rift en veille ? manettes éteintes ?)" : ""));
            }
            Sleep(4);  // ~250 Hz; SteamVR interpolates between updates
        }
    }
};

RiftTracking::RiftTracking() : impl_(std::make_unique<Impl>()) {}
RiftTracking::~RiftTracking() { stop(); }

bool RiftTracking::start(const std::filesystem::path& dataDir) {
    auto& d = *impl_;
    d.dir = dataDir;
    // Invisible: no rendering, the Rift is only a tracking source.
    ovrInitParams p{ovrInit_RequestVersion | ovrInit_Invisible | ovrInit_FocusAware, OVR_MINOR_VERSION, nullptr, 0, 0};
    if (const ovrResult r = ovr_Initialize(&p); OVR_FAILURE(r)) {
        ovrErrorInfo info{};
        ovr_GetLastErrorInfo(&info);
        d.status(r == -3008 ? std::string("Rift : le pilote Oculus intégré à SteamVR utilise déjà le Rift. Fermer SteamVR, cliquer « 2. Préparer SteamVR », relancer.")
                            : std::string("Rift : runtime Meta indisponible (Meta Horizon Link installé et lancé ?) [") + std::to_string(r) + " " + info.ErrorString + "]");
        return false;
    }
    ovrGraphicsLuid luid;
    if (const ovrResult r = ovr_Create(&d.session, &luid); OVR_FAILURE(r)) {
        ovrErrorInfo info{};
        ovr_GetLastErrorInfo(&info);
        ovr_Shutdown();
        d.session = nullptr;
        d.status(std::string("Rift : casque CV1 non détecté (branché, Meta Horizon Link lancé ?) [") + std::to_string(r) + " " + info.ErrorString + "]");
        return false;
    }
    ovr_SetTrackingOriginType(d.session, ovrTrackingOrigin_FloorLevel);
    d.loadCalibration();
    std::error_code ec;  // a request left over from an earlier session must not fire now
    std::filesystem::remove(dataDir / L"calibrate.request", ec);
    for (auto& t : d.trackers) vr::VRServerDriverHost()->TrackedDeviceAdded(t.serial(), vr::TrackedDeviceClass_GenericTracker, &t);
    d.running = true;
    d.thread = std::thread([&d] { d.loop(); });
    d.status(d.calib.valid ? "Rift connecté, calibration chargée" : "Rift connecté : calibration nécessaire");
    return true;
}

bool RiftTracking::started() const { return impl_->running; }

void RiftTracking::stop() {
    auto& d = *impl_;
    if (!d.running.exchange(false)) return;
    if (d.thread.joinable()) d.thread.join();
    ovr_Destroy(d.session);
    ovr_Shutdown();
    d.session = nullptr;
}

void RiftTracking::setReference(const protocol::ControllerState& c, double predictionS) {
    impl_->predictS = predictionS > 0 && predictionS < 0.2 ? predictionS : 0.04;
    auto& d = *impl_;
    std::lock_guard l(d.m);
    d.refValid = (c.flags & protocol::kPoseValid) != 0;
    d.refTime = ovr_GetTimeInSeconds();
    d.refP = {c.pose.position[0], c.pose.position[1], c.pose.position[2]};
    d.refQ = {c.pose.orientation[3], c.pose.orientation[0], c.pose.orientation[1], c.pose.orientation[2]};
}

}  // namespace kage::driver
