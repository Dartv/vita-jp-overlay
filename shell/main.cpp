/* Loads the RCO as a paf plugin once SceShell's paf is running (called from
 * boot.c's hook). */
#include <limits>
#include <paf.h>

extern "C" {
#include "shell.h"
}

using namespace paf;

static void plugin_start_cb(Plugin *plugin)
{
    if (plugin)
        vjo_overlay_init(plugin);
}

extern "C" void vjo_plugin_load(void)
{
    Plugin::InitParam param;
    param.name = "vitajpoverlay_plugin";
    param.resource_file = VJO_RCO_PATH;
    param.caller_name = "__main__";
    param.start_func = plugin_start_cb;
    Plugin::LoadSync(param);
}
