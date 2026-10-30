#include <QtTest>
#include <piezo_spectrum.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <limits>
#include <numeric>

namespace {
const double pi=std::acos(-1.);
PiezoResultSeries::Dataset samples(const QVector<double> &values,double dt=.01)
{
    PiezoResultSeries::Dataset dataset;dataset.family="layered";dataset.sourceFingerprint=QString(64,'a');dataset.configurationIdentity=QString(64,'b');dataset.backendIdentity=QString(64,'c');dataset.artifactSha256=QString(64,'d');
    for(qsizetype i=0;i<values.size();++i){auto frame=std::make_shared<PiezoResultSeries::Frame>();frame->family=dataset.family;frame->sourceFingerprint=dataset.sourceFingerprint;frame->configurationIdentity=dataset.configurationIdentity;frame->backendIdentity=dataset.backendIdentity;
        frame->completedFE=uint(i);frame->targetSteps=uint(values.size()-1);frame->timeS=double(i)*dt;frame->dtS=dt;frame->initialTimeS=0;
        frame->channels.append({"piezo:port:sense:readout:absolute","piezoReadout","absolute","V","accepted-named-piezo-port","sense",0,false,values[i]});
        dataset.frames.append(frame);dataset.records.append({QString::number(i)+".rbm",QString(64,'e'),uint(i)});
    }return dataset;
}
PiezoSpectrum::Result calculate(const PiezoResultSeries::Dataset &dataset,PiezoSpectrum::Window window=PiezoSpectrum::Window::Rectangular,bool removeMean=false)
{return PiezoSpectrum::calculate(dataset,"piezo:port:sense:readout:absolute",{window,removeMean});}
bool close(double a,double b,double tolerance=1e-10){return std::abs(a-b)<=tolerance*std::max({1.,std::abs(a),std::abs(b)});}
// Independent O(N^2) direct definition, not a call to the production FFT.
QVector<std::complex<double>> directDFT(QVector<double> values,bool hann,bool removeMean)
{
    const int n=values.size();const double mean=removeMean?std::accumulate(values.begin(),values.end(),0.)/n:0.;QVector<std::complex<double>> bins;
    for(int k=0;k<=n/2;++k){std::complex<double> value;
        for(int j=0;j<n;++j){const double angle=-2*pi*j*k/n,weight=hann?.5-.5*std::cos(2*pi*j/n):1.;value+=(values[j]-mean)*weight*std::complex<double>(std::cos(angle),std::sin(angle));}bins.append(value);
    }return bins;
}
}

class TestPiezoSpectrum:public QObject
{
    Q_OBJECT
private slots:
    void arbitraryOddEvenLengthMatchesIndependentDFT_data()
    {
        QTest::addColumn<int>("count");QTest::addColumn<bool>("hann");QTest::addColumn<bool>("removeMean");
        for(int count:{4,5,6,7,16,17})for(bool hann:{false,true})for(bool mean:{false,true})QTest::newRow(qPrintable(QString("n%1-window%2-mean%3").arg(count).arg(hann).arg(mean)))<<count<<hann<<mean;
    }
    void arbitraryOddEvenLengthMatchesIndependentDFT()
    {
        QFETCH(int,count);QFETCH(bool,hann);QFETCH(bool,removeMean);QVector<double> values;for(int i=0;i<count;++i)values.append(.7+std::sin(2*pi*2*i/count)+.3*std::cos(2*pi*i/count)+.05*i);
        const auto dataset=samples(values);const auto result=calculate(dataset,hann?PiezoSpectrum::Window::PeriodicHann:PiezoSpectrum::Window::Rectangular,removeMean);QVERIFY2(result.valid,qPrintable(result.message));const auto &s=result.spectrum;
        const auto expected=directDFT(values,hann,removeMean);double gain=0,power=0;for(int i=0;i<count;++i){const double weight=hann?.5-.5*std::cos(2*pi*i/count):1.;gain+=weight;power+=weight*weight;}
        QCOMPARE(s.frequenciesHz.size(),count/2+1);for(int k=0;k<expected.size();++k){const double doubling=k>0&&!(count%2==0&&k==count/2)?2:1,mag=std::abs(expected[k]);
            QVERIFY(close(s.frequenciesHz[k],100.*k/count));QVERIFY(close(s.amplitude[k],doubling*mag/gain));QVERIFY(close(s.psd[k],doubling*std::norm(expected[k])/(100*power)));
        }
        QCOMPARE(s.sourceFingerprint,dataset.sourceFingerprint);QCOMPARE(s.artifactSha256,dataset.artifactSha256);QCOMPARE(s.sampleCount,qsizetype(count));QCOMPARE(s.amplitudeUnit,QString("V"));QCOMPARE(s.psdUnit,QString("(V)^2/Hz"));QVERIFY(s.sampledCoverage);
    }
    void knownSinusoidDCNyquistAndParseval()
    {
        const int count=64;QVector<double> values;for(int i=0;i<count;++i)values.append(1.25+3.2*std::sin(2*pi*7*i/count)+.8*(i%2?-1.:1.));const auto dataset=samples(values,.001);
        for(auto window:{PiezoSpectrum::Window::Rectangular,PiezoSpectrum::Window::PeriodicHann})for(bool removeMean:{false,true}){
            const auto result=calculate(dataset,window,removeMean);QVERIFY2(result.valid,qPrintable(result.message));const auto &s=result.spectrum;
            QVERIFY(close(s.amplitude[0],removeMean?0:1.25));QVERIFY(close(s.amplitude[7],3.2));QVERIFY(close(s.amplitude[count/2],.8));QCOMPARE(s.nyquistHz,500.);QCOMPARE(s.binWidthHz,1000./count);
            double expectedPower=0,windowPower=0;for(int i=0;i<count;++i){const double weight=window==PiezoSpectrum::Window::PeriodicHann?.5-.5*std::cos(2*pi*i/count):1.,value=values[i]-(removeMean?1.25:0.);expectedPower+=value*value*weight*weight;windowPower+=weight*weight;}
            const double integrated=std::accumulate(s.psd.begin(),s.psd.end(),0.)*s.binWidthHz;QVERIFY(close(integrated,expectedPower/windowPower));
        }
        const auto constant=samples(QVector<double>(count,3.));const auto retained=calculate(constant),removed=calculate(constant,PiezoSpectrum::Window::Rectangular,true);QVERIFY(retained.valid);QVERIFY(removed.valid);QCOMPARE(retained.spectrum.amplitude[0],3.);for(double value:removed.spectrum.amplitude)QCOMPARE(value,0.);
    }
    void twentyThousandAndOddBoundaryUseAllSamples_data(){QTest::addColumn<int>("count");QTest::newRow("20000")<<20000;QTest::newRow("20001")<<20001;}
    void twentyThousandAndOddBoundaryUseAllSamples()
    {
        QFETCH(int,count);QVector<double> values;for(int i=0;i<count;++i)values.append(2.*std::sin(2*pi*137*i/count));const auto result=calculate(samples(values),PiezoSpectrum::Window::Rectangular,true);QVERIFY2(result.valid,qPrintable(result.message));
        QCOMPARE(result.spectrum.sampleCount,qsizetype(count));QCOMPARE(result.spectrum.amplitude.size(),count/2+1);QVERIFY(close(result.spectrum.amplitude[137],2.,2e-9));
        QCOMPARE(std::max_element(result.spectrum.amplitude.begin(),result.spectrum.amplitude.end())-result.spectrum.amplitude.begin(),qsizetype(137));
    }
    void compactColumnsMatchAllOriginalSamplesAndRejectMalformedGrid() {
        for(int count:{5,6,20001}){
            QVector<double> values;for(int i=0;i<count;++i)values.append(.25+std::sin(2*pi*2*i/count));const auto legacy=samples(values);
            PiezoResultSeries::NumericDataset compact;compact.family=legacy.family;compact.sourceFingerprint=legacy.sourceFingerprint;compact.configurationIdentity=legacy.configurationIdentity;compact.backendIdentity=legacy.backendIdentity;compact.artifactSha256=legacy.artifactSha256;compact.dtS=.01;compact.targetSteps=quint64(count-1);
            compact.channels=legacy.frames.first()->channels;compact.channels.first().value=0;compact.columns.append(values);for(int i=0;i<count;++i){compact.times.append(i*.01);compact.completedFE.append(uint(i));}
            for(auto window:{PiezoSpectrum::Window::Rectangular,PiezoSpectrum::Window::PeriodicHann})for(bool mean:{false,true}){
                const auto expected=calculate(legacy,window,mean),actual=PiezoSpectrum::calculate(compact,compact.channels.first().channelId,{window,mean});QVERIFY2(actual.valid,qPrintable(actual.message));QVERIFY(expected.valid);
                QCOMPARE(actual.spectrum.frequenciesHz,expected.spectrum.frequenciesHz);QCOMPARE(actual.spectrum.amplitude,expected.spectrum.amplitude);QCOMPARE(actual.spectrum.psd,expected.spectrum.psd);QCOMPARE(actual.spectrum.artifactSha256,compact.artifactSha256);QCOMPARE(actual.spectrum.sampleCount,qsizetype(count));
            }
            const auto check=[&](const PiezoResultSeries::NumericDataset &bad){QVERIFY(!PiezoSpectrum::calculate(bad,compact.channels.first().channelId,{}).valid);};
            auto bad=compact;bad.completedFE[2]++;check(bad);bad=compact;bad.times[2]+=.001;check(bad);bad=compact;bad.columns.first().removeLast();check(bad);bad=compact;bad.channels.append(bad.channels.first());bad.columns.append(values);check(bad);bad=compact;bad.baseSeq=1;check(bad);bad=compact;bad.artifactSha256.clear();check(bad);bad=compact;bad.channels.first().unit="m";check(bad);bad=compact;bad.columns.first()[2]=std::numeric_limits<double>::quiet_NaN();check(bad);
        }
    }
    void unsupportedCoverageClockIdentityChannelAndRangeAreRejected()
    {
        const auto good=samples({1.,2.,3.,4.,5.});QVERIFY(calculate(good).valid);
        const auto mutate=[&](const std::function<void(PiezoResultSeries::Frame&)> &change){auto altered=good;auto frame=std::make_shared<PiezoResultSeries::Frame>(*good.frames[2]);change(*frame);altered.frames[2]=frame;return altered;};
        auto altered=good;altered.continuousBetweenSelectedRecords=false;QVERIFY(!calculate(altered).valid);
        altered=good;for(qsizetype i=0;i<altered.frames.size();++i){auto frame=std::make_shared<PiezoResultSeries::Frame>(*good.frames[i]);frame->completedFE=uint(2*i);frame->targetSteps=8;frame->timeS=2*i*frame->dtS;altered.frames[i]=frame;altered.records[i].completedFE=frame->completedFE;}QVERIFY(!calculate(altered).valid); // Uniformly spaced holes are still rejected.
        QVERIFY(!calculate(mutate([](auto &frame){++frame.completedFE;frame.timeS+=frame.dtS;})).valid);
        QVERIFY(!calculate(mutate([](auto &frame){frame.timeS+=.0001;})).valid);QVERIFY(!calculate(mutate([](auto &frame){frame.dtS*=2;})).valid);
        QVERIFY(!calculate(mutate([](auto &frame){frame.sourceFingerprint="changed";})).valid);QVERIFY(!calculate(mutate([](auto &frame){frame.configurationIdentity="changed";})).valid);QVERIFY(!calculate(mutate([](auto &frame){frame.backendIdentity="changed";})).valid);
        QVERIFY(!calculate(mutate([](auto &frame){frame.channels.first().unit="C";})).valid);QVERIFY(!calculate(mutate([](auto &frame){frame.channels.clear();})).valid);
        QVERIFY(!calculate(mutate([](auto &frame){frame.channels.first().value=std::numeric_limits<double>::infinity();})).valid);
        altered=good;altered.records.clear();QVERIFY(!calculate(altered).valid);altered=good;altered.artifactSha256.clear();QVERIFY(!calculate(altered).valid);
        QVERIFY(!calculate(samples({1.,2.,3.})).valid);QVERIFY(!calculate(samples(QVector<double>(20002,1.))).valid);
        QVERIFY(!calculate(samples(QVector<double>(4,1.),1e-310)).valid);
        // Finite input may have an unrepresentable PSD. Reject explicitly.
        QVERIFY(!calculate(samples(QVector<double>(4,1e200))).valid);
        const auto large=calculate(samples(QVector<double>(8,1e155),1e-9));QVERIFY2(large.valid,qPrintable(large.message));QVERIFY(close(large.spectrum.amplitude[0]/1e155,1.));QVERIFY(std::isfinite(large.spectrum.psd[0]));
        const auto small=calculate(samples(QVector<double>(8,1e-140)));QVERIFY2(small.valid,qPrintable(small.message));QVERIFY(small.spectrum.psd[0]>0);
    }
};
QTEST_GUILESS_MAIN(TestPiezoSpectrum)
#include "tst_piezo_spectrum.moc"
