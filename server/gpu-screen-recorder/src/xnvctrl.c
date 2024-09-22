#include "../include/xnvctrl.h"
#include "../include/library_loader.h"
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

bool gsr_xnvctrl_load(gsr_xnvctrl *self, Display *display) {
    memset(self, 0, sizeof(gsr_xnvctrl));
    self->display = display;

    dlerror(); /* clear */
    void *lib = dlopen("libXNVCtrl.so.0", RTLD_LAZY);
    if(!lib) {
        fprintf(stderr, "gsr error: gsr_xnvctrl_load failed: failed to load libXNVCtrl.so.0, error: %s\n", dlerror());
        return false;
    }

    const dlsym_assign required_dlsym[] = {
        { (void**)&self->XNVCTRLQueryExtension, "XNVCTRLQueryExtension" },
        { (void**)&self->XNVCTRLSetTargetAttributeAndGetStatus, "XNVCTRLSetTargetAttributeAndGetStatus" },
        { (void**)&self->XNVCTRLQueryValidTargetAttributeValues, "XNVCTRLQueryValidTargetAttributeValues" },
        { (void**)&self->XNVCTRLQueryTargetStringAttribute, "XNVCTRLQueryTargetStringAttribute" },

        { NULL, NULL }
    };

    if(!dlsym_load_list(lib, required_dlsym)) {
        fprintf(stderr, "gsr error: gsr_xnvctrl_load failed: missing required symbols in libXNVCtrl.so.0\n");
        goto fail;
    }

    self->library = lib;
    return true;

    fail:
    dlclose(lib);
    memset(self, 0, sizeof(gsr_xnvctrl));
    return false;
}

void gsr_xnvctrl_unload(gsr_xnvctrl *self) {
    if(self->library) {
        dlclose(self->library);
        memset(self, 0, sizeof(gsr_xnvctrl));
    }
}
