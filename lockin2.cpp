#include "lockin2.hpp"
#include <cmath>
#include <QDebug>

Lockin2::Lockin2(QObject *parent) :
    QObject(parent)
{
    _buffer = new QBuffer(this);
    _buffer->open(QIODevice::ReadWrite);

    _inputStream.setDevice(_buffer);

    _audioInput = 0;

    setOutputPeriod(0.5);
    setVumeterTime(0.02); // dix periodes de 500Hz
    setIntegrationTime(3.0);
    setPhase(0.0);
}

bool Lockin2::isFormatSupported(const QAudioFormat &format)
{
    if (format.codec() != "audio/pcm") {
        return false;
    }

    if (format.channelCount() != 2) {
        return false;
    }

    if (format.sampleType() != QAudioFormat::UnSignedInt) {
        return false;
    }

    if (format.sampleSize() != 32) {
        return false;
    }

    return true;
}

bool Lockin2::start(const QAudioDeviceInfo &audioDevice, const QAudioFormat &format)
{
    if (_audioInput != 0) {
        qDebug() << __FUNCTION__ << ": lockin is already running, please stop is before start";
        return false;
    }

    if (!format.isValid()) {
        qDebug() << __FUNCTION__ << ": format not valid";
        return false;
    }

    if (!isFormatSupported(format)) {
        qDebug() << __FUNCTION__ << ": format not supported for lockin2";
        return false;
    }

    if (audioDevice.isFormatSupported(format)) {
        _audioInput = new QAudioInput(audioDevice, format, this);
        _audioInput->setNotifyInterval(_outputPeriod * 1000.0);
        connect(_audioInput, SIGNAL(notify()), this, SLOT(interpretInput()));

        _inputStream.setByteOrder((QDataStream::ByteOrder)format.byteOrder());

        // pour être au millieu avec le temps
        _timeValue = -(_integrationTime / 2.0);

        // nombre d'échantillons pour le temps d'integration
        _sampleIntegration = format.sampleRate() * _integrationTime;

        // nombre d'échantillons pour un affichage de vumeter
        _sampleVumeter = format.sampleRate() * _vumeterTime;

        _audioInput->start(_buffer);
    } else {
        qDebug() << __FUNCTION__ << ": format not supported, can't start";
        return false;
    }


    return true;
}

void Lockin2::setOutputPeriod(qreal outputPeriod)
{
    if (_audioInput == 0)
        _outputPeriod = outputPeriod;
    else
        qDebug() << __FUNCTION__ << ": lockin is running";
}

void Lockin2::setIntegrationTime(qreal integrationTime)
{
    if (_audioInput == 0)
        _integrationTime = integrationTime;
    else
        qDebug() << __FUNCTION__ << ": lockin is running";
}

void Lockin2::setVumeterTime(qreal vumeterTime)
{
    if (_audioInput == 0)
        _vumeterTime = vumeterTime;
    else
        qDebug() << __FUNCTION__ << ": lockin is running";
}

void Lockin2::setPhase(qreal phase)
{
    _phase = phase;
}

qreal Lockin2::autoPhase() const
{
    if (_audioInput != 0) {
        return _phase + std::atan2(_yValue, _xValue);
    } else {
        qDebug() << __FUNCTION__ << ": lockin is not running";
        return 0.0;
    }
}

QList<QPair<qreal, qreal> > Lockin2::vumeterData()
{
    _vumeterMutex.lock();
    QList<QPair<qreal, qreal> > ret = _vumeterData;
    _vumeterMutex.unlock();
    return ret;
}

void Lockin2::stop()
{
    if (_audioInput != 0) {
        delete _audioInput;
        _audioInput = 0;
    } else {
        qDebug() << __FUNCTION__ << ": lockin is not running";
    }
}

void Lockin2::interpretInput()
{
    _timeValue += _outputPeriod;

    // récupère les nouvelles valeurs
    /*
     * le nombre de nouvelle valeurs = outputPeriod * sampleRate / 1000
     * outputPeriod (~0.1 s) étant beaucoup plus grand que la periode du chopper (~1/500 s)
     * meme dans le cas d'un chopper lent et d'un temps de sortie rapide le nombre de periodes
     * du chopper reste elevé. (~50)
     * On peut donc garder uniquement les periodes entières
     * On a donc une perte de présision de l'ordre de 2/50 = 4%
     * Plus outputPeriod et grand et plus le chopper est rapide plus la perte diminue.
     * 0.5s 500Hz -> 0.8%
     * 1.0s 500Hz -> 0.4%
     */
    QList<qreal> leftSignal;
    QList<quint32> chopperSignal;

    int newSamplesCount = 0;
    quint64 avgRight = 0;

    while (_buffer->bytesAvailable() >= qint64(2 * sizeof (quint32))) {
        quint32 left, right;

        _inputStream >> left;
        _inputStream >> right;


        leftSignal << left;
        chopperSignal << right;

        avgRight += right;
        newSamplesCount++;
    }

    if (newSamplesCount == 0) {
        qDebug() << __FUNCTION__ << ": nothing new...";
        return;
    }

    avgRight /= newSamplesCount;

    QList<QPair<qreal, qreal> > chopperSinCos = parseChopperSignal(chopperSignal, avgRight, _phase);

    // multiplie le leftSignal (>0) par chopperSinCos ([-1;1])
    for (int i = 0; i < leftSignal.size(); ++i) {
        qreal x, y;
        if (chopperSinCos[i].first != 2.0) { // 2.0 est la valeur qui indique ignoreValue dans parseChopperSignal(...)
            x = chopperSinCos[i].first * leftSignal[i]; // sin
            y = chopperSinCos[i].second * leftSignal[i]; // cos
        } else {
            x = y = 0.5; // valeur impossible sin(x) = cos(x) = ~0.707 et comme leftSignal est issue de nombres entiers
            // la plus petite valeur positive avec (x = y) est 0.707
        }
        _data << QPair<qreal, qreal>(x, y);
    }

    // renouvelle le view data (prend la fin des listes, qui correspond aux parties les plus récentes)
    if (_vumeterMutex.tryLock()) {
        _vumeterData.clear();
        for (int i = leftSignal.size() - 1; i >= 0; --i) {
            if (_vumeterData.size() >= _sampleVumeter)
                break;

            if (chopperSinCos[i].first != 2.0) {
                _vumeterData.prepend(QPair<qreal, qreal>(leftSignal[i], chopperSinCos[i].first));
            }
        }
        _vumeterMutex.unlock();
    } else {
        qDebug() << __FUNCTION__ << ": the view Mutex is locked";
    }



    // le nombre d'échantillons correslondant au temps d'integration

    if (_data.size() < _sampleIntegration) {
        // pas encore assez d'elements pour faire la moyenne
        return;
    }

    // supprime les valeurs en trop
    while (_data.size() > _sampleIntegration)
        _data.removeFirst();

    qreal dataCount = 0.0;
    qreal x = 0.0;
    qreal y = 0.0;

    for (int i = 0; i < _data.size(); ++i) {
        // deux fois 0.5 indique ignoreValue
        if (_data[i].first != 0.5 && _data[i].second != 0.5) {
            x += _data[i].first;
            y += _data[i].second;
            dataCount += 1.0;
        }
    }
    x /= dataCount;
    y /= dataCount;

    //    _values << QPair<qreal, QPair<qreal, qreal> >(_timeValue, QPair<qreal, qreal>(x, y));
    _xValue = x;
    _yValue = y;

    emit newValues(_timeValue, x, y);
}

QList<QPair<qreal, qreal> > Lockin2::parseChopperSignal(QList<quint32> signal, quint32 avg, qreal phase)
{
    // transforme le signal en sin et cos déphasé
    // dans les bords, la valeur 2.0 indique d'on ignore l'element

    QList<QPair<qreal, qreal> > out;

    bool ignore = true;
    bool wasBiggerThanAvg = (signal.first() > avg);
    int periodSize = 0;

    for (int i = 0; i < signal.size(); ++i) {
        bool isBiggerThanAvg = (signal[i] > avg);

        periodSize += 1;

        if (isBiggerThanAvg == true && wasBiggerThanAvg == false) {
            // flanc montant

            if (ignore) {
                // que pour les premières valeurs
                for (int i = 0; i < periodSize; ++i)
                    out << QPair<qreal, qreal>(2.0, 2.0);
                ignore = false;
            } else {
                // construire le sin et le cos
                for (int i = 0; i < periodSize; ++i) {
                    qreal angle = M_2_PI * (qreal)i / (qreal)periodSize + phase;
                    out << QPair<qreal, qreal>(std::sin(angle), std::cos(angle));
                }
            }

            periodSize = 0;
        }

        wasBiggerThanAvg = isBiggerThanAvg;
    }

    while (out.size() < signal.size())
        out << QPair<qreal, qreal>(2.0, 2.0);

    return out;
}
