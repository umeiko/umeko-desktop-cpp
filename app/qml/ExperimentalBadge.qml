import QtQuick

Rectangle {
    implicitWidth: label.implicitWidth + 12
    implicitHeight: label.implicitHeight + 6
    radius: 4
    color: Theme.dark ? "#514315" : "#fff3bf"
    border.color: Theme.dark ? "#8e7424" : "#e3c46a"
    Text {
        id: label
        anchors.centerIn: parent
        text: I18n.t("实验版"); textFormat: Text.PlainText
        color: Theme.dark ? "#ffdc75" : "#865900"
        font.family: Theme.fontFamily
        font.pixelSize: Theme.font(10); font.weight: Font.Medium
    }
}
