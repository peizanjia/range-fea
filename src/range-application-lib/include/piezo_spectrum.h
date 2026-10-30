#ifndef PIEZO_SPECTRUM_H
#define PIEZO_SPECTRUM_H
#include "piezo_result_series.h"

//! Pure DFT analysis of immutable certified named-port records. This helper
//! does not authenticate files; the caller retains Reader/source/hash guards.
class PiezoSpectrum
{
public:
    enum class Window { PeriodicHann, Rectangular };
    struct Options {Window window=Window::PeriodicHann;bool removeMean=true;};
    struct Spectrum
    {
        QString sourceFingerprint,configurationIdentity,backendIdentity,artifactSha256;
        QString channelId,portId,quantity,reference,amplitudeUnit,psdUnit,window;
        QString coverage;
        bool removeMean=true,actionRecord=false,sampledCoverage=true;
        uint firstFE=0,lastFE=0;
        qsizetype sampleCount=0;
        double firstTimeS=0,lastTimeS=0,dtS=0,samplingRateHz=0,binWidthHz=0,nyquistHz=0;
        QVector<double> frequenciesHz,amplitude,psd;
    };
    struct Result {bool valid=false;QString message;Spectrum spectrum;};
    //! All original samples enter the DFT; no interpolation or display reduction.
    //! A consecutive subwindow is permitted and remains explicitly sampled coverage.
    static Result calculate(const PiezoResultSeries::Dataset &dataset,const QString &channelId,
                            const Options &options);
    static Result calculate(const PiezoResultSeries::NumericDataset &dataset,const QString &channelId,
                            const Options &options);
};
#endif
