#include "piezo_spectrum.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using Complex=std::complex<double>;
const double pi=std::acos(-1.);
void require(bool condition,const char *message){if(!condition)throw std::invalid_argument(message);}
// Standard DFT convention: X[k]=sum_j x[j] exp(-2*pi*i*j*k/N).
// Iterative radix-2 butterflies; inverse has the opposite sign and a 1/N factor.
void radix2(std::vector<Complex> &values,bool inverse)
{
    const size_t n=values.size();
    for(size_t i=1,j=0;i<n;++i){size_t bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j)std::swap(values[i],values[j]);}
    for(size_t width=2;width<=n;width*=2){const auto rotation=std::polar(1.,(inverse?2.:-2.)*pi/double(width));
        for(size_t start=0;start<n;start+=width){Complex phase(1,0);for(size_t j=0;j<width/2;++j){
            const auto even=values[start+j],odd=phase*values[start+j+width/2];values[start+j]=even+odd;values[start+j+width/2]=even-odd;phase*=rotation;
        }}
    }
    if(inverse)for(auto &value:values)value/=double(n);
}
std::vector<Complex> transform(const std::vector<double> &input)
{
    const size_t n=input.size();
    if((n&(n-1))==0){std::vector<Complex> result(input.begin(),input.end());radix2(result,false);return result;}
    // jk=(j*j+k*k-(j-k)*(j-k))/2 gives Bluestein's convolution.
    // Chirps are reduced modulo 2N before evaluating their phase; no large angles.
    size_t size=1;while(size<2*n-1)size*=2;
    std::vector<Complex> a(size),b(size),chirp(n);
    for(size_t j=0;j<n;++j){const double angle=pi*double((j*j)%(2*n))/double(n);chirp[j]=std::polar(1.,-angle);
        a[j]=input[j]*chirp[j];b[j]=std::conj(chirp[j]);if(j)b[size-j]=b[j];}
    radix2(a,false);radix2(b,false);for(size_t j=0;j<size;++j)a[j]*=b[j];radix2(a,true);
    a.resize(n);for(size_t k=0;k<n;++k)a[k]*=chirp[k];return a;
}
bool sameSignal(const PiezoResultSeries::Signal &a,const PiezoResultSeries::Signal &b)
{
    return a.channelId==b.channelId&&a.portId==b.portId&&a.quantity==b.quantity&&a.axis==b.axis
        &&a.unit==b.unit&&a.actionRecord==b.actionRecord&&a.nodeTarget==b.nodeTarget&&a.physicalNodeId==b.physicalNodeId;
}
double finite(double value,const char *message){require(std::isfinite(value),message);return value;}
void computeBins(PiezoSpectrum::Spectrum &spectrum,std::vector<double> values,double maximum,const PiezoSpectrum::Options &options)
{
    const qsizetype count=qsizetype(values.size());
        // Scale before summation/FFT so finite large signals cannot overflow an
        // intermediate transform. Restore units only at the final amplitude/PSD.
        if(maximum>0)for(auto &value:values)value/=maximum;
        double mean=0,correction=0;if(options.removeMean){for(double value:values){const double adjusted=value-correction,next=mean+adjusted;correction=(next-mean)-adjusted;mean=next;}mean/=double(count);}
        double gain=0,power=0;for(qsizetype i=0;i<count;++i){const double weight=options.window==PiezoSpectrum::Window::PeriodicHann?.5-.5*std::cos(2*pi*double(i)/double(count)):1.;
            values[size_t(i)]=(values[size_t(i)]-mean)*weight;gain+=weight;power+=weight*weight;}
        const auto bins=transform(values);const qsizetype size=count/2+1;
        spectrum.frequenciesHz.reserve(size);spectrum.amplitude.reserve(size);spectrum.psd.reserve(size);
        for(qsizetype k=0;k<size;++k){const double magnitude=std::abs(bins[size_t(k)]);const bool doubled=k>0&&!(count%2==0&&k==count/2);
            const double amplitude=finite((magnitude/gain*(doubled?2.:1.))*maximum,"Spectrum amplitude exceeds the finite supported range");
            const double root=magnitude/std::sqrt(power)/std::sqrt(spectrum.samplingRateHz)*maximum;
            const double psd=finite(root*root*(doubled?2.:1.),"Spectrum PSD exceeds the finite supported range");
            require(magnitude==0||maximum==0||psd>0,"Spectrum PSD underflows the supported floating-point range");
            spectrum.frequenciesHz.append(spectrum.samplingRateHz*(double(k)/double(count)));spectrum.amplitude.append(amplitude);spectrum.psd.append(psd);
        }
}
}

PiezoSpectrum::Result PiezoSpectrum::calculate(const PiezoResultSeries::Dataset &dataset,const QString &channelId,const Options &options)
{
    Result result;
    try {
        const qsizetype count=dataset.frames.size();require(count>=4&&count<=20001,"Frequency analysis requires 4..20001 original accepted samples");
        require(dataset.continuousBetweenSelectedRecords,"Frequency analysis rejects gaps in accepted FE steps; interpolation is not performed");
        require(!dataset.sourceFingerprint.isEmpty()&&!dataset.configurationIdentity.isEmpty()&&!dataset.backendIdentity.isEmpty()&&!dataset.artifactSha256.isEmpty(),"Spectrum requires bound source, configuration, backend and artifact identities");
        require(dataset.records.size()==count,"Spectrum record proofs must match the original samples");
        require(options.window==Window::PeriodicHann||options.window==Window::Rectangular,"Unsupported spectrum window");
        Spectrum spectrum;spectrum.sourceFingerprint=dataset.sourceFingerprint;spectrum.configurationIdentity=dataset.configurationIdentity;
        spectrum.backendIdentity=dataset.backendIdentity;spectrum.artifactSha256=dataset.artifactSha256;spectrum.sampleCount=count;
        spectrum.coverage=dataset.coverage;spectrum.sampledCoverage=dataset.sampledCoverage;spectrum.removeMean=options.removeMean;
        spectrum.window=options.window==Window::PeriodicHann?"periodic Hann":"rectangular";
        std::vector<double> values;values.reserve(size_t(count));PiezoResultSeries::Signal identity;
        require(bool(dataset.frames.first()),"Accepted spectrum frame is missing");const auto &first=*dataset.frames.first();
        spectrum.dtS=first.dtS;require(std::isfinite(spectrum.dtS)&&spectrum.dtS>0,"Spectrum sample interval must be finite and positive");
        spectrum.samplingRateHz=finite(1./spectrum.dtS,"Spectrum sampling rate cannot be represented");
        spectrum.binWidthHz=finite(spectrum.samplingRateHz/double(count),"Spectrum bin width cannot be represented");
        require(spectrum.binWidthHz>0,"Spectrum bin width underflows the supported range");spectrum.nyquistHz=spectrum.samplingRateHz*.5;
        spectrum.firstTimeS=first.timeS;spectrum.firstFE=first.completedFE;
        double maximum=0;
        for(qsizetype i=0;i<count;++i){require(bool(dataset.frames[i]),"Accepted spectrum frame is missing");const auto &frame=*dataset.frames[i];
            require(frame.family==dataset.family&&frame.sourceFingerprint==dataset.sourceFingerprint&&frame.configurationIdentity==dataset.configurationIdentity&&frame.backendIdentity==dataset.backendIdentity,"Spectrum frames differ in source, family, configuration or backend");
            require(frame.dtS==spectrum.dtS&&frame.initialTimeS==first.initialTimeS&&frame.targetSteps==first.targetSteps&&frame.completedFE<=frame.targetSteps,"Spectrum original FE clock changed");
            require(std::isfinite(frame.timeS)&&std::isfinite(frame.initialTimeS),"Spectrum time is nonfinite");
            require(dataset.records[i].completedFE==frame.completedFE&&!dataset.records[i].sha256.isEmpty(),"Spectrum accepted record proof changed");
            const double expected=first.initialTimeS+double(frame.completedFE)*spectrum.dtS;
            const double roundoff=16*std::numeric_limits<double>::epsilon()*std::max({std::abs(expected),std::abs(frame.timeS),spectrum.dtS});
            require(std::isfinite(expected)&&std::abs(frame.timeS-expected)<=roundoff,"Spectrum sample time differs from its original FE clock");
            if(i){const auto &previous=*dataset.frames[i-1];require(frame.completedFE==previous.completedFE+1,"Frequency analysis rejects gaps in accepted FE steps; interpolation is not performed");
                const double interval=frame.timeS-previous.timeS;
                const double tolerance=1e-9*spectrum.dtS+16*std::numeric_limits<double>::epsilon()*std::max(std::abs(frame.timeS),std::abs(previous.timeS));
                require(interval>0&&std::abs(interval-spectrum.dtS)<=tolerance,"Frequency analysis requires a uniform original time grid; interpolation is not performed");
            }
            const auto signal=std::find_if(frame.channels.begin(),frame.channels.end(),[&](const auto &s){return s.channelId==channelId;});
            require(signal!=frame.channels.end(),"Named port channel is missing from an accepted sample");
            require(std::count_if(frame.channels.begin(),frame.channels.end(),[&](const auto &s){return s.channelId==channelId;})==1,"Named port channel is duplicated");
            if(!i){identity=*signal;require(!identity.portId.isEmpty()&&identity.physicalNodeId==0&&(identity.unit=="V"||identity.unit=="C"),"Spectrum requires an actual named electrode port with its V or C unit");}
            else require(sameSignal(identity,*signal),"Spectrum channel, mode, action or physical unit changed between samples");
            values.push_back(finite(signal->value,"Spectrum port value is nonfinite"));maximum=std::max(maximum,std::abs(signal->value));
        }
        spectrum.lastTimeS=dataset.frames.last()->timeS;spectrum.lastFE=dataset.frames.last()->completedFE;
        spectrum.channelId=identity.channelId;spectrum.portId=identity.portId;spectrum.quantity=identity.quantity;spectrum.reference=identity.axis;
        spectrum.actionRecord=identity.actionRecord;spectrum.amplitudeUnit=identity.unit;spectrum.psdUnit="("+identity.unit+")^2/Hz";
        computeBins(spectrum,std::move(values),maximum,options);
        result.spectrum=std::move(spectrum);result.valid=true;
    }catch(const std::exception &error){result.message=QString::fromUtf8(error.what());}
    return result;
}

PiezoSpectrum::Result PiezoSpectrum::calculate(const PiezoResultSeries::NumericDataset &dataset,const QString &channelId,const Options &options)
{
    Result result;
    try {
        const qsizetype count=dataset.times.size();
        require(count>=4&&count<=20001,"Frequency analysis requires 4..20001 original accepted samples");
        require(dataset.continuousBetweenSelectedRecords,"Frequency analysis rejects gaps in accepted FE steps; interpolation is not performed");
        require(!dataset.family.isEmpty()&&!dataset.sourceFingerprint.isEmpty()&&!dataset.configurationIdentity.isEmpty()&&!dataset.backendIdentity.isEmpty()&&!dataset.artifactSha256.isEmpty(),"Spectrum requires bound source, configuration, backend and artifact identities");
        require(dataset.completedFE.size()==count&&dataset.channels.size()==dataset.columns.size(),"Compact spectrum sample/catalog dimensions differ");
        require(options.window==Window::PeriodicHann||options.window==Window::Rectangular,"Unsupported spectrum window");
        const auto found=std::find_if(dataset.channels.begin(),dataset.channels.end(),[&](const auto &s){return s.channelId==channelId;});
        require(found!=dataset.channels.end(),"Named port channel is missing from accepted columns");
        require(std::count_if(dataset.channels.begin(),dataset.channels.end(),[&](const auto &s){return s.channelId==channelId;})==1,"Named port channel is duplicated");
        const auto &identity=*found;
        require(!identity.portId.isEmpty()&&identity.physicalNodeId==0&&(identity.unit=="V"||identity.unit=="C"),"Spectrum requires an actual named electrode port with its V or C unit");
        const auto &column=dataset.columns[found-dataset.channels.begin()];require(column.size()==count,"Compact spectrum column length differs from original samples");
        Spectrum spectrum;spectrum.sourceFingerprint=dataset.sourceFingerprint;spectrum.configurationIdentity=dataset.configurationIdentity;spectrum.backendIdentity=dataset.backendIdentity;spectrum.artifactSha256=dataset.artifactSha256;
        spectrum.coverage=dataset.coverage;spectrum.sampledCoverage=dataset.sampledCoverage;spectrum.sampleCount=count;spectrum.removeMean=options.removeMean;spectrum.window=options.window==Window::PeriodicHann?"periodic Hann":"rectangular";
        spectrum.dtS=dataset.dtS;require(std::isfinite(dataset.initialTimeS)&&std::isfinite(dataset.dtS)&&dataset.dtS>0,"Spectrum original FE clock is invalid");
        spectrum.samplingRateHz=finite(1./dataset.dtS,"Spectrum sampling rate cannot be represented");spectrum.binWidthHz=finite(spectrum.samplingRateHz/double(count),"Spectrum bin width cannot be represented");require(spectrum.binWidthHz>0,"Spectrum bin width underflows the supported range");spectrum.nyquistHz=spectrum.samplingRateHz*.5;
        require(dataset.completedFE.first()==dataset.baseSeq,"Compact spectrum baseline differs from its committed prefix");
        std::vector<double> values;values.reserve(size_t(count));double maximum=0;
        for(qsizetype i=0;i<count;++i){const auto step=dataset.completedFE[i];require(step<=dataset.targetSteps,"Spectrum original FE clock changed");
            const double time=finite(dataset.times[i],"Spectrum time is nonfinite"),expected=dataset.initialTimeS+double(step)*dataset.dtS;
            const double roundoff=16*std::numeric_limits<double>::epsilon()*std::max({std::abs(expected),std::abs(time),dataset.dtS});
            require(std::isfinite(expected)&&std::abs(time-expected)<=roundoff,"Spectrum sample time differs from its original FE clock");
            if(i){require(quint64(step)==quint64(dataset.completedFE[i-1])+1,"Frequency analysis rejects gaps in accepted FE steps; interpolation is not performed");
                const double interval=time-dataset.times[i-1],tolerance=1e-9*dataset.dtS+16*std::numeric_limits<double>::epsilon()*std::max(std::abs(time),std::abs(dataset.times[i-1]));
                require(interval>0&&std::abs(interval-dataset.dtS)<=tolerance,"Frequency analysis requires a uniform original time grid; interpolation is not performed");}
            values.push_back(finite(column[i],"Spectrum port value is nonfinite"));maximum=std::max(maximum,std::abs(column[i]));
        }
        spectrum.firstFE=dataset.completedFE.first();spectrum.lastFE=dataset.completedFE.last();spectrum.firstTimeS=dataset.times.first();spectrum.lastTimeS=dataset.times.last();
        spectrum.channelId=identity.channelId;spectrum.portId=identity.portId;spectrum.quantity=identity.quantity;spectrum.reference=identity.axis;spectrum.actionRecord=identity.actionRecord;spectrum.amplitudeUnit=identity.unit;spectrum.psdUnit="("+identity.unit+")^2/Hz";
        computeBins(spectrum,std::move(values),maximum,options);result.spectrum=std::move(spectrum);result.valid=true;
    }catch(const std::exception &error){result.message=QString::fromUtf8(error.what());}
    return result;
}
