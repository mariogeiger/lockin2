#include <QApplication>
#include <lockin2/lockin2gui.hpp>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    Lockin2Gui l;
    l.show();

    return a.exec();
}
