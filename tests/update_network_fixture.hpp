#pragma once
#include <QtNetwork>
#include <cstring>

class UpdateReply : public QNetworkReply {
  QByteArray payload;
  qint64 offset = 0;

public:
  UpdateReply(const QNetworkRequest &request, QByteArray data, int status,
              QObject *parent)
      : QNetworkReply(parent), payload(std::move(data)) {
    setRequest(request);
    setUrl(request.url());
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
    if (status >= 400)
      setError(QNetworkReply::ContentNotFoundError, "Fixture HTTP failure");
    else if (status == 0)
      setError(QNetworkReply::ConnectionRefusedError, "Fixture connection failure");
    open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    QTimer::singleShot(0, this, [this] {
      if (isFinished())
        return;
      emit readyRead();
      if (isFinished())
        return;
      setFinished(true);
      emit finished();
    });
  }
  void abort() override {
    if (isFinished())
      return;
    setError(QNetworkReply::OperationCanceledError, "Cancelled");
    setFinished(true);
    emit finished();
  }
  qint64 bytesAvailable() const override {
    return payload.size() - offset + QNetworkReply::bytesAvailable();
  }

protected:
  qint64 readData(char *target, qint64 maximum) override {
    const auto count = qMin(maximum, payload.size() - offset);
    if (count <= 0)
      return -1;
    std::memcpy(target, payload.constData() + offset, size_t(count));
    offset += count;
    return count;
  }
};

class UpdateNetworkFixture : public QNetworkAccessManager {
public:
  QQueue<QPair<QByteArray, int>> responses;
  QList<QUrl> urls;
  void add(QByteArray body, int status = 200) {
    responses.enqueue({body, status});
  }

protected:
  QNetworkReply *createRequest(Operation, const QNetworkRequest &request,
                               QIODevice *) override {
    urls << request.url();
    const auto response = responses.isEmpty() ? qMakePair(QByteArray(), 404)
                                              : responses.dequeue();
    return new UpdateReply(request, response.first, response.second, this);
  }
};
