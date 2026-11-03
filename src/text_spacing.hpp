#pragma once
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>

class TextSpacing : public QObject {
  Q_OBJECT
  Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY changed)
  Q_PROPERTY(double factor READ factor WRITE setFactor NOTIFY changed)
  Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
public:
  explicit TextSpacing(QObject *parent=nullptr):QObject(parent) {}
  QQuickTextDocument *document() const {return quick;}
  double factor() const {return spacing;}
  bool enabled() const {return active;}
  void setDocument(QQuickTextDocument *value) {
    if(quick==value)return;
    if(quick)disconnect(quick->textDocument(),nullptr,this,nullptr);
    quick=value;
    if(quick)connect(quick->textDocument(),&QTextDocument::contentsChange,this,[this]{apply();});
    apply();emit changed();
  }
  void setFactor(double value) {if(!qIsFinite(value) || value<.5 || value>3 || qFuzzyCompare(spacing,value))return;spacing=value;apply();emit changed();}
  void setEnabled(bool value) {if(active==value)return;active=value;apply();emit changed();}
signals:
  void changed();
private:
  QPointer<QQuickTextDocument> quick;
  double spacing=1.;
  bool active=true,applying=false;
  void apply() {
    if(!quick || !active || applying)return;
    applying=true;const auto guard=qScopeGuard([this]{applying=false;});
    auto document=quick->textDocument();QTextCursor cursor(document);cursor.beginEditBlock();
    for(auto block=document->begin();block.isValid();block=block.next()) {
      auto format=block.blockFormat();const auto height=spacing*100;
      if(format.lineHeightType()!=QTextBlockFormat::ProportionalHeight || !qFuzzyCompare(format.lineHeight(),height)) {
        format.setLineHeight(height,QTextBlockFormat::ProportionalHeight);cursor.setPosition(block.position());cursor.setBlockFormat(format);
      }
    }
    cursor.endEditBlock();
  }
};
