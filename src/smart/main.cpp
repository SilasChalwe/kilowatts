#if defined(DEVICE_ROLE_SMART)
#include "SmartApplication.h"

extern "C" void app_main()
{
    SmartApplication app;
    app.runApp();
}

#endif // DEVICE_ROLE_SMART

