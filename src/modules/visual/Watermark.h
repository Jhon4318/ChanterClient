#pragma once

#include "modules/module.h"

class Watermark final : public Module {
public:
    Watermark();
    void on_imgui_render() override;
};
