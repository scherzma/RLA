#include "Analyzer.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <format>

namespace RLA {
namespace {
double Median(std::vector<double> values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) * 0.5;
}
double Interval(const std::vector<MouseEvent>& events, double frequency) {
    std::vector<double> values;
    for (size_t i = 1; i < events.size(); ++i)
        if (events[i].timestamp > events[i - 1].timestamp)
            values.push_back((events[i].timestamp - events[i - 1].timestamp) * 1000.0 / frequency);
    return Median(std::move(values));
}
struct Minimum { size_t index; double time, quality; };
struct Curve {
    std::vector<double> x, y, magnitude, smooth;
    std::vector<size_t> invalid;
    std::vector<Minimum> minima;
    size_t smoothingRadius = 0;
    bool Covers(size_t first, size_t last) const {
        // Include every report used by the centered smoothing window. A gap
        // just outside the fit must not leak artificial zeros into its edge.
        return first >= smoothingRadius && first <= last && last + smoothingRadius < magnitude.size() &&
            invalid[last + smoothingRadius + 1] == invalid[first - smoothingRadius];
    }
};
Curve MakeCurve(const RecordingSession& session, const std::vector<MouseEvent>& events, size_t size, double step) {
    Curve c;
    c.x.resize(size); c.y.resize(size); c.magnitude.resize(size); c.smooth.resize(size);
    std::vector<bool> covered(size);
    for (const auto& bin : Analyzer::BinMovement(events, session.startTimestamp, session.qpcFrequency, step, true)) {
        if (bin.index < 0 || static_cast<size_t>(bin.index) >= size) continue;
        c.x[bin.index] = bin.x; c.y[bin.index] = bin.y;
        c.magnitude[bin.index] = std::hypot(bin.x, bin.y);
    }
    const double gap = (std::max)(3.0, 6 * Interval(events, session.qpcFrequency));
    for (size_t i = 1; i < events.size(); ++i) {
        const double begin = (events[i - 1].timestamp - session.startTimestamp) * 1000.0 / session.qpcFrequency;
        const double end = (events[i].timestamp - session.startTimestamp) * 1000.0 / session.qpcFrequency;
        if (end - begin > gap) continue;
        for (size_t j = static_cast<size_t>(begin / step); j <= static_cast<size_t>(end / step) && j < size; ++j)
            covered[j] = true;
    }
    c.invalid.resize(size + 1);
    std::vector<double> prefix(size + 1);
    for (size_t i = 0; i < size; ++i) {
        prefix[i + 1] = prefix[i] + c.magnitude[i];
        c.invalid[i + 1] = c.invalid[i] + !covered[i];
    }
    // Use the same centered time window for both mice. A few report intervals
    // alone measure arrival jitter rather than the shape of a movement cycle.
    const size_t smoothRadius = (std::max)(size_t(1), static_cast<size_t>(std::round(3.0 / step)));
    c.smoothingRadius=smoothRadius;
    for (size_t i = 0; i < size; ++i) {
        const size_t lo = i > smoothRadius ? i - smoothRadius : 0, hi = (std::min)(size - 1, i + smoothRadius);
        c.smooth[i] = (prefix[hi + 1] - prefix[lo]) / (hi - lo + 1);
    }
    const int radius = static_cast<int>(std::ceil(10.0 / step));
    for (size_t i = radius; i + radius < size; ++i) {
        if (c.smooth[i] > c.smooth[i - 1] || c.smooth[i] >= c.smooth[i + 1] || !c.Covers(i - radius, i + radius)) continue;
        const double shoulder = (std::min)(c.smooth[i - radius], c.smooth[i + radius]);
        if (shoulder < 2 || c.smooth[i] > shoulder * 0.5 || shoulder - c.smooth[i] < 1.5) continue;
        // Near a smooth reversal, squared speed is approximately quadratic.
        // Fit the whole bottom, not the timestamp of the smallest noisy sample.
        double x2 = 0, x4 = 0, y = 0, xy = 0, x2y = 0, yy = 0;
        const int n = 2 * radius + 1;
        for (int j = -radius; j <= radius; ++j) {
            const double x = double(j) / radius;
            const double v = std::pow(c.smooth[i + j] / shoulder, 2);
            x2 += x*x; x4 += x*x*x*x; y += v; xy += x*v; x2y += x*x*v; yy += v*v;
        }
        const double a = (x2y - x2 * y / n) / (x4 - x2 * x2 / n), b = xy / x2;
        const double constant = (y - a * x2) / n;
        const double offset = a > 0 ? -b / (2*a) : 0;
        const double error = (std::max)(0.0, yy - a*x2y - b*xy - constant*y);
        const double variance = yy - y*y/n;
        const double quality = variance > 0 ? 1 - error / variance : 0;
        const bool precise = a > 0 && std::abs(offset) <= 0.4 && quality >= 0.90 && std::sqrt(error / n) <= 0.16;
        // A trough can locate the surrounding slopes even when its bottom is
        // too noisy or broad to give a reliable minimum timestamp.
        Minimum point{i, (i + 0.5 + (precise ? offset * radius : 0)) * step, precise ? quality : 0};
        if (!c.minima.empty() && point.time - c.minima.back().time < 12) {
            if (point.quality > c.minima.back().quality || (point.quality == c.minima.back().quality && c.smooth[i] < c.smooth[c.minima.back().index])) c.minima.back() = point;
        } else c.minima.push_back(point);
    }
    return c;
}
double MatchQuality(const Curve& a, const Minimum& ma, const Curve& b, const Minimum& mb, double step) {
    const int radius = static_cast<int>(std::ceil(10 / step));
    if (ma.index < radius || mb.index < radius || !a.Covers(ma.index-radius, ma.index+radius) ||
        !b.Covers(mb.index-radius, mb.index+radius)) return 0;
    double aa=0, bb=0, ab=0, sa=0, sb=0;
    int active=0, same=0;
    const int n = 2*radius+1;
    for (int j=-radius; j<=radius; ++j) {
        const size_t ia=ma.index+j, ib=mb.index+j;
        const double va=a.smooth[ia], vb=b.smooth[ib];
        sa+=va; sb+=vb; aa+=va*va; bb+=vb*vb; ab+=va*vb;
        const double norm=std::hypot(a.x[ia],a.y[ia])*std::hypot(b.x[ib],b.y[ib]);
        if (norm > 1) { ++active; if (a.x[ia]*b.x[ib]+a.y[ia]*b.y[ib] > norm*0.8) ++same; }
    }
    if (active < n/3 || same < active*0.8 || aa<=sa*sa/n || bb<=sb*sb/n) return 0;
    return (ab-sa*sb/n)/std::sqrt((aa-sa*sa/n)*(bb-sb*sb/n));
}
struct Crossing { bool valid=false; double time=0, slope=0; };
Crossing HalfHeight(const Curve& c, size_t minimum, bool rising, double step) {
    if ((!rising && minimum==0) || (rising && minimum+1==c.minima.size())) return {};
    const size_t center=c.minima[minimum].index;
    const size_t other=c.minima[rising?minimum+1:minimum-1].index;
    const size_t first=(std::min)(center,other), last=(std::max)(center,other);
    if ((last-first)*step > 300 || !c.Covers(first,last)) return {};
    const size_t peak=std::max_element(c.smooth.begin()+first,c.smooth.begin()+last+1)-c.smooth.begin();
    const double amplitude=c.smooth[peak]-c.smooth[center];
    if (amplitude<3) return {};
    const double target=c.smooth[center]+amplitude*0.5;
    size_t cross=center;
    if (rising) { while (cross<peak && c.smooth[cross]<target) ++cross; }
    else { while (cross>peak && c.smooth[cross]<target) --cross; }
    const int r=(std::max)(2,static_cast<int>(std::ceil(1.0/step)));
    if (cross<r || !c.Covers(cross-r,cross+r)) return {};
    double sum=0, xy=0, xx=0, yy=0;
    for (int j=-r;j<=r;++j) {
        const double x=j*step, y=(c.smooth[cross+j]-target)/amplitude;
        sum+=y; xy+=x*y; xx+=x*x; yy+=y*y;
    }
    const double slope=xy/xx, intercept=sum/(2*r+1);
    if (std::abs(slope)<0.004 || (rising ? slope<=0 : slope>=0)) return {};
    const double offset=-intercept/slope;
    if (std::abs(offset)>2*step || std::sqrt((std::max)(0.0,yy-slope*xy-intercept*sum)/(2*r+1))>0.04) return {};
    return {true,(cross+0.5)*step+offset,slope};
}
}

LatencyFit Analyzer::FitLatency(const RecordingSession& session, int featureMode) {
    LatencyFit fit;
    fit.message="No reliable estimate. Record at least three clean, shared movement cycles.";
    if (!std::isfinite(session.qpcFrequency) || session.qpcFrequency<=0 || session.startTimestamp<0 ||
        session.endTimestamp<=session.startTimestamp || session.eventsA.size()<30 || session.eventsB.size()<30) return fit;
    for (const auto* events : {&session.eventsA,&session.eventsB}) {
        int64_t previous=session.startTimestamp;
        for (const auto& event:*events) {
            if (event.timestamp<previous || event.timestamp>session.endTimestamp) return fit;
            previous=event.timestamp;
        }
    }
    const double interval=(std::max)(Interval(session.eventsA,session.qpcFrequency),Interval(session.eventsB,session.qpcFrequency));
    if (interval<=0 || interval>2) { fit.message="Report spacing is too large for this latency fit."; return fit; }
    fit.binMs=(std::max)(0.25,std::round(interval/0.125)*0.125);
    const double count=std::ceil((session.endTimestamp-session.startTimestamp)*1000.0/session.qpcFrequency/fit.binMs)+1;
    if (!std::isfinite(count) || count>2000000) { fit.message="Recording is too long for latency fitting. Use a shorter recording."; return fit; }
    const auto a=MakeCurve(session,session.eventsA,static_cast<size_t>(count),fit.binMs);
    const auto b=MakeCurve(session,session.eventsB,static_cast<size_t>(count),fit.binMs);
    std::vector<size_t> cycles;
    for (size_t i=0;i<a.minima.size();++i) {
        const auto& ma=a.minima[i];
        const auto it=std::lower_bound(b.minima.begin(),b.minima.end(),ma.time,[](const Minimum& m,double t){return m.time<t;});
        size_t j=it-b.minima.begin();
        if (j && (j==b.minima.size() || ma.time-b.minima[j-1].time<b.minima[j].time-ma.time)) --j;
        if (j==b.minima.size() || std::abs(b.minima[j].time-ma.time)>10) continue;
        const auto& mb=b.minima[j];
        if ((i && std::abs(a.minima[i-1].time-mb.time)<std::abs(ma.time-mb.time)) ||
            (i+1<a.minima.size() && std::abs(a.minima[i+1].time-mb.time)<std::abs(ma.time-mb.time))) continue;
        ++fit.candidatePairs;
        const double quality=MatchQuality(a,ma,b,mb,fit.binMs);
        if (quality<0.95) continue;
        auto add=[&](double ta,double tb,LatencyFeature feature) {
            if (std::abs(tb-ta)>10) return;
            fit.matches.push_back({ta,tb,tb-ta,quality,feature}); cycles.push_back(i);
        };
        if (featureMode!=1 && ma.quality>0 && mb.quality>0) add(ma.time,mb.time,LatencyFeature::Minimum);
        if (featureMode!=0) for (bool rising:{false,true}) {
            const auto ca=HalfHeight(a,i,rising,fit.binMs), cb=HalfHeight(b,j,rising,fit.binMs);
            if (ca.valid && cb.valid && ca.slope/cb.slope>0.67 && ca.slope/cb.slope<1.5 &&
                MatchQuality(a, {static_cast<size_t>(ca.time/fit.binMs),ca.time,0},
                    b, {static_cast<size_t>(cb.time/fit.binMs),cb.time,0}, fit.binMs)>=0.98)
                add(ca.time,cb.time,rising?LatencyFeature::Rising:LatencyFeature::Falling);
        }
    }
    if (fit.matches.size()<3) {
        fit.message=std::format("No reliable estimate: {} candidate pairs, {} accepted features. Shape, direction, or feature-quality checks failed.",fit.candidatePairs,fit.matches.size());
        return fit;
    }
    std::vector<double> differences;
    for (const auto& match:fit.matches) differences.push_back(match.differenceMs);
    const double middle=Median(differences);
    std::vector<double> deviations;
    for (double d:differences) deviations.push_back(std::abs(d-middle));
    const double limit=(std::max)(2*fit.binMs,3*1.4826*Median(deviations));
    std::vector<LatencyMatch> accepted;
    std::vector<size_t> acceptedCycles;
    for (size_t i=0;i<fit.matches.size();++i) if (std::abs(fit.matches[i].differenceMs-middle)<=limit) {
        accepted.push_back(fit.matches[i]); acceptedCycles.push_back(cycles[i]);
    }
    if (accepted.size()*3<fit.matches.size()*2) return fit;
    fit.matches=std::move(accepted);
    std::vector<double> votes;
    for (size_t i=0;i<fit.matches.size();) {
        size_t j=i; std::vector<double> cycle;
        while (j<fit.matches.size() && acceptedCycles[j]==acceptedCycles[i]) cycle.push_back(fit.matches[j++].differenceMs);
        votes.push_back(Median(cycle)); i=j;
    }
    fit.matchedCycles=votes.size();
    if (votes.size()<3) {
        fit.message=std::format("Only {} matching cycles passed. At least three are required.",votes.size());
        return fit;
    }
    fit.differenceMs=Median(votes);
    deviations.clear();
    for (const auto& match:fit.matches) deviations.push_back(std::abs(match.differenceMs-fit.differenceMs));
    fit.spreadMs=Median(deviations);
    if (fit.spreadMs>(std::max)(1.0,2*fit.binMs)) { fit.message="Matched features disagree. No reliable latency estimate."; return fit; }
    fit.valid=true;
    fit.message="Positive B-A means B appears later. This includes motion and application timing; it is not isolated USB latency.";
    return fit;
}
} // namespace RLA
