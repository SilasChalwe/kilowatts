#if defined(DEVICE_ROLE_CENTRAL)
#include "CentralApplication.h"

extern "C" void app_main()
{
    CentralApplication app;
    app.runApp();
}

#endif // DEVICE_ROLE_CENTRAL
