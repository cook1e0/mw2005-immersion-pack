#include "../src/spectator_camera.h"
#include "../src/spectator_motion.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>

// Independent ray oracle: apply quaternion products in double precision, without
// using the controller's coverage predicate or matrix conversion.
struct DQ { double x, y, z, w; };
static DQ Mul(DQ a, DQ b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y, a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
            a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w, a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
static DQ D(Quat q) { return {q.x,q.y,q.z,q.w}; }
static DQ Inv(DQ q) { return {-q.x,-q.y,-q.z,q.w}; }
static double Extent(const SpectatorCamera& out, const SpectatorCameraInput& in) {
    DQ q = Mul(Inv(D(in.eye)), D(out.rotation));
    double worst = 0;
    for (int x : {-1,1}) for (int y : {-1,1}) {
        DQ p = Mul(Mul(q, {x*out.tanX,y*out.tanY,-1,0}), Inv(q));
        if (p.z >= -1e-6) return 1e9;
        worst = std::max(worst, std::max(fabs(p.x / p.z / in.sourceTanX), fabs(p.y / p.z / in.sourceTanY)));
    }
    return worst;
}
static const double kPi = 3.14159265358979323846;
static float Rad(double d) { return (float)(d*kPi/180); }
static double Deg(double r) { return r*180/kPi; }
static Quat Pose(double y, double p = 0, double r = 0) {
    return QMul(QMul(QFromAxisAngle({0,1,0},Rad(y)),QFromAxisAngle({1,0,0},Rad(p))),QFromAxisAngle({0,0,1},Rad(r)));
}
static DQ Rotated(Quat q, double x, double y, double z) { return Mul(Mul(D(q),{x,y,z,0}),Inv(D(q))); }
static double Yaw(Quat q) { DQ f=Rotated(q,0,0,-1); return atan2(-f.x,-f.z); }
static double Pitch(Quat q) { DQ f=Rotated(q,0,0,-1); return asin(std::max(-1.0,std::min(f.y,1.0))); }
static double Roll(Quat q) {
    double y=Yaw(q),p=Pitch(q);
    DQ level=Mul({0,sin(y/2),0,cos(y/2)},{sin(p/2),0,0,cos(p/2)});
    DQ r=Mul(Inv(level),D(q));
    return remainder(2*atan2(r.z,r.w),2*kPi);
}
static double Delta(double a,double b) { return remainder(a-b,2*kPi); }
static SpectatorCameraInput Input(Quat q = {0,0,0,1}, float x = 1.3f, float y = 1.2f) {
    return {q,x,y,1832,1920,1.0f/90};
}
static uint64_t checked=0;
static double worstExtent=0;
static void Check(const SpectatorCamera& out,const SpectatorCameraInput& in) {
    double extent=Extent(out,in);
    if (!(extent <= 1.000001)) {
        printf("FAIL coverage %.9f: source %.7f %.7f, crop %.7f %.7f, eye %.7f %.7f %.7f %.7f, out %.7f %.7f %.7f %.7f\n",
               extent,in.sourceTanX,in.sourceTanY,out.tanX,out.tanY,in.eye.x,in.eye.y,in.eye.z,in.eye.w,
               out.rotation.x,out.rotation.y,out.rotation.z,out.rotation.w);
        fflush(stdout);
    }
    assert(extent <= 1.000001);
    assert(out.tanX > 0 && out.tanY > 0);
    double n=out.rotation.x*out.rotation.x+out.rotation.y*out.rotation.y+out.rotation.z*out.rotation.z+out.rotation.w*out.rotation.w;
    assert(fabs(n-1)<0.00001);
    worstExtent=std::max(worstExtent,extent);
    checked++;
}
static uint32_t seed=0x53ec7a70;
static float Random(float lo,float hi) { seed=1664525*seed+1013904223; return lo+(hi-lo)*(seed>>8)/16777216.0f; }

static void CropRegression() {
    SpectatorController controller;
    SpectatorCameraInput in = {{0,0,0,1}, 0.6f, 0.4f, 1832, 1920, 0};
    SpectatorCameraOptions opt;
    auto out = controller.Step(in, opt);
    printf("low rendered FOV: final corner extent %.6f (must be <= 1)\n", Extent(out, in));
    fflush(stdout);
    Check(out,in);
    assert(fabs(out.tanX/out.tanY-opt.aspect)<1e-5);
    // Narrow output aspects must fit source height even when source width is ample.
    opt.aspect=0.5f;
    out=controller.Step(in,opt);
    Check(out,in);
    assert(out.tanY <= in.sourceTanY);
}
static void RandomCoverage() {
    SpectatorController controller;
    SpectatorCameraOptions opt;
    for (int i=0;i<40000;i++) {
        SpectatorCameraInput in=Input(Pose(Random(-360,360),Random(-89.99f,89.99f),Random(-180,180)),
                                      Random(0.2f,3.5f),Random(0.2f,3.5f));
        in.width=(int)Random(320,4096);
        in.height=(int)Random(240,4096);
        in.dt=i%97==0 ? 0.25f : Random(1.0f/240,1.0f/30);
        in.reset=i%193==0;
        opt.fov=Random(30,155);
        opt.aspect=Random(0.25f,4.0f);
        opt.smoothing=Random(0,1);
        opt.adaptive=i%2==0;
        opt.followHead=i%17!=0;
        auto out=controller.Step(in,opt);
        Check(out,in);
        assert(fabs(out.tanX/out.tanY-opt.aspect)<0.00001);
    }
    printf("random coverage: 40000 samples, seeded yaw/pitch/roll, changing source FOV/aspect/resolution\n");
}
static void LevelAndRelease() {
    SpectatorController c;
    SpectatorCameraOptions opt;
    auto in=Input(Pose(20,15,30),2,2);
    auto out=c.Step(in,opt);
    Check(out,in);
    assert(fabs(Deg(Roll(out.rotation)))<0.01);
    assert(fabs(Deg(Delta(Yaw(out.rotation),Rad(20))))<0.01);
    assert(fabs(Deg(Pitch(out.rotation))-15)<0.01);

    // The almost-height-filling crop cannot be level at a 30-degree source lean.
    SpectatorController tight;
    in=Input(Pose(0,0,30),1.2f,0.5f);
    for(int i=0;i<90;i++) { out=tight.Step(in,opt); Check(out,in); }
    double forced=fabs(Deg(Roll(out.rotation)));
    assert(forced>20 && forced<=30.01);
    SpectatorCamera lessRoll=out;
    lessRoll.rotation=Pose(0,0,forced-0.2);
    assert(Extent(lessRoll,in)>1); // The required roll is close to the first covered angle.
    in=Input(Pose(0),1.2f,1.5f);
    out=tight.Step(in,opt); Check(out,in);
    double released=fabs(Deg(Roll(out.rotation)));
    assert(released>0.1 && released<forced);
    for(int i=0;i<150;i++) { out=tight.Step(in,opt); Check(out,in); }
    assert(fabs(Deg(Roll(out.rotation)))<0.01);
    printf("roll: level when covered; required %.3f deg, first release %.3f deg, final %.5f deg\n",forced,released,Deg(Roll(out.rotation)));
}
static void ResetsAndWrap() {
    SpectatorController c;
    SpectatorCameraOptions opt;
    auto in=Input(Pose(179),3,3);
    auto out=c.Step(in,opt); Check(out,in);
    in.eye=Pose(-179);
    out=c.Step(in,opt); Check(out,in);
    assert(fabs(Deg(Delta(Yaw(out.rotation),Rad(179))))<2);
    assert(fabs(Deg(Yaw(out.rotation)))>175);
    in.eye=Pose(40,-15); in.dt=0.25f;
    out=c.Step(in,opt); Check(out,in);
    assert(fabs(Deg(Delta(Yaw(out.rotation),Rad(40))))<0.01);
    assert(fabs(Deg(Pitch(out.rotation))+15)<0.01);
    in.eye=Pose(-40,15); in.dt=0;
    out=c.Step(in,opt); Check(out,in);
    assert(fabs(Deg(Delta(Yaw(out.rotation),Rad(-40))))<0.01);
    in.eye=Pose(20); in.dt=1.0f/90; in.reset=true;
    out=c.Step(in,opt); Check(out,in);
    assert(fabs(Deg(Delta(Yaw(out.rotation),Rad(20))))<0.01);
    puts("state: yaw wrap, zero dt, long gap, and explicit reset passed");
}
struct Metrics { double jitter, lag; };
static Metrics FilterMetrics(bool adaptive) {
    SpectatorController c;
    SpectatorCameraOptions opt; opt.adaptive=adaptive;
    auto in=Input(Pose(0),3,3);
    double sumJitter=0,sumLag=0; int nJitter=0,nLag=0;
    for(int i=0;i<1080;i++) {
        double t=i/90.0;
        double trueYaw=t<4 ? 0 : t<5 ? (t-4)*80 : 80;
        double noise=0.2*sin(2*kPi*13*t);
        in.eye=Pose(trueYaw+noise);
        auto out=c.Step(in,opt); Check(out,in);
        double output=Deg(Yaw(out.rotation));
        if(t>1 && t<4) { sumJitter+=output*output; nJitter++; }
        if(t>=4 && t<5) { sumLag+=fabs(output-trueYaw); nLag++; }
    }
    Metrics m={sqrt(sumJitter/nJitter),sumLag/nLag};
    // Raw following has ~0.141-degree RMS jitter on this signal.
    assert(m.jitter<0.04);
    assert(m.lag>0.1); // Do not accept raw-eye passthrough as stabilization.
    return m;
}
static void MotionCoverage() {
    SpectatorController c;
    SpectatorCameraOptions opt; opt.adaptive=true;
    for(int i=0;i<1440;i++) {
        double t=i/90.0, yaw=0,pitch=0,roll=0;
        if(t<4) yaw=65*sin(2*kPi*t/4);
        else if(t<8) pitch=60*sin(2*kPi*(t-4)/4);
        else if(t<10) yaw=t<9 ? 0 : 85;
        else if(t<12) pitch=t<11 ? 0 : 65;
        else { yaw=70*sin(2*kPi*(t-12)); roll=30*sin(kPi*(t-12)/4); }
        auto in=Input(Pose(yaw,pitch,roll));
        auto out=c.Step(in,opt); Check(out,in);
    }
    // Coverage correction must feed back: returning from a sustained constrained look
    // settles near the new heading rather than releasing accumulated hidden lag.
    for(int i=0;i<180;i++) {
        auto in=Input(Pose(0));
        auto out=c.Step(in,opt); Check(out,in);
        if(i==179) assert(fabs(Deg(Yaw(out.rotation)))<0.05);
    }
    puts("motion coverage: yaw/pitch sweeps, yaw/pitch step flicks, 30-degree lean with yaw, and recovery passed");
}
static void CorrectionFeedback() {
    SpectatorController c;
    SpectatorCameraOptions opt; opt.smoothing=0.5f;
    auto in=Input(Pose(0));
    auto out=c.Step(in,opt); Check(out,in);
    in.eye=Pose(80);
    out=c.Step(in,opt); Check(out,in);
    double constrained=Deg(Yaw(out.rotation));
    assert(constrained>60);
    in.eye=Pose(70);
    out=c.Step(in,opt); Check(out,in);
    // The new 70-degree look contains the previous displayed orientation. A filter
    // that retained the old near-zero yaw would jump ~10 degrees back to the edge.
    assert(Deg(Yaw(out.rotation))>constrained-1);
    printf("state feedback: constrained %.3f deg, next contained look %.3f deg\n",constrained,Deg(Yaw(out.rotation)));
}
static void SingularitiesAndTemporalChanges() {
    SpectatorController c;
    SpectatorCameraOptions opt; opt.adaptive=true;
    for(float pitch : {-90.0f,-89.999f,89.999f,90.0f})
        for(float yaw : {-180.0f,-90.0f,0.0f,90.0f,180.0f})
            for(float roll : {-180.0f,-90.0f,0.0f,90.0f,180.0f}) {
                auto in=Input(Pose(yaw,pitch,roll),1.2f,0.7f);
                auto out=c.Step(in,opt); Check(out,in);
            }
    double y=0,p=0,r=0;
    auto in=Input();
    for(int i=0;i<5000;i++) {
        y+=Random(-2,2); p=std::max(-89.99,std::min(p+Random(-2,2),89.99)); r+=Random(-3,3);
        in.eye=Pose(y,p,r);
        if(i%37==0) {
            in.sourceTanX=Random(0.4f,2);
            in.sourceTanY=Random(0.35f,2);
            opt.aspect=Random(0.5f,3);
            opt.fov=Random(45,110);
        }
        in.dt=Random(1.0f/144,1.0f/45);
        auto out=c.Step(in,opt); Check(out,in);
    }
    puts("poses: exact/near vertical singularities and 5000 correlated temporal samples passed");
}
static void SharedFakeHmdMotion() {
    for(int hz : {72,90,120}) {
        SpectatorController c;
        SpectatorCameraOptions opt; opt.adaptive=true;
        for(int i=0;i<12*hz;i++) {
            auto in=Input(Spectator_TestPose(i/(double)hz),1,1);
            in.dt=1.0f/hz;
            auto out=c.Step(in,opt); Check(out,in);
        }
    }
    puts("FakeHmd: shared 12-second production motion at 72, 90, and 120 Hz passed");
}
int main() {
    CropRegression();
    RandomCoverage();
    LevelAndRelease();
    ResetsAndWrap();
    MotionCoverage();
    CorrectionFeedback();
    SingularitiesAndTemporalChanges();
    SharedFakeHmdMotion();
    Metrics exponential=FilterMetrics(false),adaptive=FilterMetrics(true);
    printf("filter comparison: exponential jitter %.5f deg RMS, ramp lag %.3f deg; adaptive jitter %.5f deg RMS, ramp lag %.3f deg\n",
           exponential.jitter,exponential.lag,adaptive.jitter,adaptive.lag);
    assert(adaptive.lag < exponential.lag*0.7);
    printf("spectator tests passed: %llu final camera samples, zero uncovered frames, worst source coordinate %.9f\n",
           (unsigned long long)checked,worstExtent);
}
