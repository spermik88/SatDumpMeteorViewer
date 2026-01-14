#pragma once

#include "common/widgets/logger_sink.h"

namespace satdump
{
    class StatusLoggerSink : public widgets::LoggerSinkWidget
    {
    private:
        std::string str;
        std::string lvl;
        bool show_bar;
        bool show_log;
        std::string cached_img_time_run_id;
        std::string cached_img_time_label;

        void draw_layer_bar();
        std::string resolve_img_time_label();
    protected:
        void receive(slog::LogMsg log);
    public:
        StatusLoggerSink();
        ~StatusLoggerSink();
        int draw();
        bool is_shown();
    };
}
