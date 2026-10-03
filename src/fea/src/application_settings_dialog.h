#ifndef APPLICATION_SETTINGS_DIALOG_H
#define APPLICATION_SETTINGS_DIALOG_H

#include <QDialog>
#include <QString>

#include "application_settings.h"

class ApplicationSettingsWidget;

class ApplicationSettingsDialog : public QDialog
{

    Q_OBJECT

    private:
        ApplicationSettingsWidget *settingsWidget = nullptr;

    public:

        //! Constructor.
        explicit ApplicationSettingsDialog(ApplicationSettings *applicationSettings,
                                           QWidget *parent = nullptr,
                                           const QString &initialTab = QString());

};

#endif // APPLICATION_SETTINGS_DIALOG_H
