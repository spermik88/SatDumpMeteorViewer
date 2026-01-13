#pragma once

#include "common/widgets/logger_sink.h"
#include <array>

namespace satdump
{
    class StatusLoggerSink : public widgets::LoggerSinkWidget
    {
    private:
        static constexpr size_t kLayerCount = 6;

        std::string str;
        std::string lvl;
        bool show_bar;
        bool show_log;
        int layer_mode = 0;
        std::array<bool, kLayerCount> layer_enabled{};

        void draw_layer_bar();
    protected:
        void receive(slog::LogMsg log);
    public:
        StatusLoggerSink();
        ~StatusLoggerSink();
        int draw();
        bool is_shown();
    };
}
