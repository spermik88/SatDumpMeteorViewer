#include "webserver.h"
#include <vector>
#include <mutex>
#include "logger.h"

// Webserver for stats
namespace webserver
{
    nng_http_server *http_server;
    nng_url *url;
    nng_http_handler *handler;
    nng_http_handler *handler_html;
    nng_http_handler *handler_polarplot;
    nng_http_handler *handler_fft;
    nng_http_handler *handler_schedule;

    bool is_active = false;

    std::mutex request_mutex;

    std::function<std::string()> handle_callback = []() -> std::string
    { return ""; };

    std::function<std::string(std::string)> handle_callback_html = [](std::string) -> std::string
    { return "Please use /api for JSON data"; };

    std::function<std::vector<uint8_t>()> handle_callback_polarplot = []() -> std::vector<uint8_t>
    { return {}; };

    std::function<std::vector<uint8_t>()> handle_callback_fft = []() -> std::vector<uint8_t>
    { return {}; };

    std::function<std::vector<uint8_t>()> handle_callback_schedule = []() -> std::vector<uint8_t>
    { return {}; };

    // HTTP Handler for stats
    void http_handle(nng_aio *aio)
    {
        std::lock_guard<std::mutex> lock(request_mutex);

        std::string jsonstr = handle_callback();

        nng_http_res *res;
        nng_http_res_alloc(&res);
        nng_http_res_copy_data(res, jsonstr.c_str(), jsonstr.size());
        nng_http_res_set_header(res, "Content-Type", "application/json; charset=utf-8");
        nng_aio_set_output(aio, 0, res);
        nng_aio_finish(aio, 0);
    }

    // HTTP Handler for HTML
    void http_handle_html(nng_aio *aio)
    {
        std::lock_guard<std::mutex> lock(request_mutex);

        std::string uri = nng_http_req_get_uri((nng_http_req *)nng_aio_get_input(aio, 0));

        std::string jsonstr = handle_callback_html(uri);

        nng_http_res *res;
        nng_http_res_alloc(&res);
        nng_http_res_copy_data(res, jsonstr.c_str(), jsonstr.size());
        nng_http_res_set_header(res, "Content-Type", "text/html; charset=utf-8");
        nng_aio_set_output(aio, 0, res);
        nng_aio_finish(aio, 0);
    }

    bool add_polarplot_handler = false;

    // HTTP Handler for HTML
    void http_handle_polarplot(nng_aio *aio)
    {
        std::lock_guard<std::mutex> lock(request_mutex);

        std::vector<uint8_t> img = handle_callback_polarplot();

        nng_http_res *res;
        nng_http_res_alloc(&res);
        nng_http_res_copy_data(res, img.data(), img.size());
        nng_http_res_set_header(res, "Content-Type", "image/jpeg");
        nng_aio_set_output(aio, 0, res);
        nng_aio_finish(aio, 0);
    }

    // HTTP Handler for HTML
    void http_handle_fft(nng_aio *aio)
    {
        std::lock_guard<std::mutex> lock(request_mutex);

        std::vector<uint8_t> img = handle_callback_fft();

        nng_http_res *res;
        nng_http_res_alloc(&res);
        nng_http_res_copy_data(res, img.data(), img.size());
        nng_http_res_set_header(res, "Content-Type", "image/jpeg");
        nng_aio_set_output(aio, 0, res);
        nng_aio_finish(aio, 0);
    }

    // HTTP Handler for Schedule
    void http_handle_schedule(nng_aio *aio)
    {
        std::lock_guard<std::mutex> lock(request_mutex);

        std::vector<uint8_t> img = handle_callback_schedule();

        nng_http_res *res;
        nng_http_res_alloc(&res);
        nng_http_res_copy_data(res, img.data(), img.size());
        nng_http_res_set_header(res, "Content-Type", "image/jpeg");
        nng_aio_set_output(aio, 0, res);
        nng_aio_finish(aio, 0);
    }

    void start(std::string http_server_url)
    {
        int ret;

        http_server_url = "http://" + http_server_url;
        if ((ret = nng_url_parse(&url, http_server_url.c_str())) != 0)
        {
            logger->error("Failed to parse HTTP URL %s : %s", http_server_url.c_str(), nng_strerror(ret));
            return;
        }

        if ((ret = nng_http_server_hold(&http_server, url)) != 0)
        {
            logger->error("Failed to hold HTTP server : %s", nng_strerror(ret));
            nng_url_free(url);
            return;
        }

        if ((ret = nng_http_handler_alloc(&handler, "/api", http_handle)) != 0)
        {
            logger->error("Failed to allocate /api handler : %s", nng_strerror(ret));
            nng_http_server_release(http_server);
            nng_url_free(url);
            return;
        }

        if ((ret = nng_http_handler_set_method(handler, "GET")) != 0)
        {
            logger->error("Failed to set /api handler method : %s", nng_strerror(ret));
            nng_http_handler_free(handler);
            nng_http_server_release(http_server);
            nng_url_free(url);
            return;
        }

        if ((ret = nng_http_server_add_handler(http_server, handler)) != 0)
        {
            logger->error("Failed to add /api handler : %s", nng_strerror(ret));
            nng_http_handler_free(handler);
            nng_http_server_release(http_server);
            nng_url_free(url);
            return;
        }

        if ((ret = nng_http_handler_alloc(&handler_html, "", http_handle_html)) != 0)
        {
            logger->error("Failed to allocate HTML handler : %s", nng_strerror(ret));
            nng_http_server_release(http_server);
            nng_url_free(url);
            return;
        }

        if ((ret = nng_http_handler_set_method(handler_html, "GET")) != 0)
        {
            logger->error("Failed to set HTML handler method : %s", nng_strerror(ret));
            nng_http_handler_free(handler_html);
            nng_http_server_release(http_server);
            nng_url_free(url);
            return;
        }

        nng_http_handler_set_tree(handler_html);

        if ((ret = nng_http_server_add_handler(http_server, handler_html)) != 0)
        {
            logger->error("Failed to add HTML handler : %s", nng_strerror(ret));
            nng_http_handler_free(handler_html);
            nng_http_server_release(http_server);
            nng_url_free(url);
            return;
        }

        if (add_polarplot_handler)
        {
            if ((ret = nng_http_handler_alloc(&handler_polarplot, "/polarplot.jpeg", http_handle_polarplot)) != 0)
            {
                logger->error("Failed to allocate polarplot handler : %s", nng_strerror(ret));
                nng_http_server_release(http_server);
                nng_url_free(url);
                return;
            }

            if ((ret = nng_http_handler_set_method(handler_polarplot, "GET")) != 0)
            {
                logger->error("Failed to set polarplot handler method : %s", nng_strerror(ret));
                nng_http_handler_free(handler_polarplot);
                nng_http_server_release(http_server);
                nng_url_free(url);
                return;
            }

            if ((ret = nng_http_server_add_handler(http_server, handler_polarplot)) != 0)
            {
                logger->error("Failed to add polarplot handler : %s", nng_strerror(ret));
                nng_http_handler_free(handler_polarplot);
                nng_http_server_release(http_server);
                nng_url_free(url);
                return;
            }

            if ((ret = nng_http_handler_alloc(&handler_fft, "/fft.jpeg", http_handle_fft)) != 0)
            {
                logger->error("Failed to allocate FFT handler : %s", nng_strerror(ret));
                nng_http_server_release(http_server);
                nng_url_free(url);
                return;
            }

            if ((ret = nng_http_handler_set_method(handler_fft, "GET")) != 0)
            {
                logger->error("Failed to set FFT handler method : %s", nng_strerror(ret));
                nng_http_handler_free(handler_fft);
                nng_http_server_release(http_server);
                nng_url_free(url);
                return;
            }

            if ((ret = nng_http_server_add_handler(http_server, handler_fft)) != 0)
            {
                logger->error("Failed to add FFT handler : %s", nng_strerror(ret));
                nng_http_handler_free(handler_fft);
                nng_http_server_release(http_server);
                nng_url_free(url);
                return;
            }

            if ((ret = nng_http_handler_alloc(&handler_schedule, "/schedule.jpeg", http_handle_schedule)) != 0)
            {
                logger->error("Failed to allocate schedule handler : %s", nng_strerror(ret));
                nng_http_server_release(http_server);
                nng_url_free(url);
                return;
            }

            if ((ret = nng_http_handler_set_method(handler_schedule, "GET")) != 0)
            {
                logger->error("Failed to set schedule handler method : %s", nng_strerror(ret));
                nng_http_handler_free(handler_schedule);
                nng_http_server_release(http_server);
                nng_url_free(url);
                return;
            }

            if ((ret = nng_http_server_add_handler(http_server, handler_schedule)) != 0)
            {
                logger->error("Failed to add schedule handler : %s", nng_strerror(ret));
                nng_http_handler_free(handler_schedule);
                nng_http_server_release(http_server);
                nng_url_free(url);
                return;
            }
        }

        if ((ret = nng_http_server_start(http_server)) != 0)
        {
            logger->error("Failed to start HTTP server : %s", nng_strerror(ret));
            nng_http_server_release(http_server);
            nng_url_free(url);
            return;
        }

        nng_url_free(url);
        is_active = true;
    }

    void stop()
    {
        if (is_active)
        {
            nng_http_server_stop(http_server);
            nng_http_server_release(http_server);
            is_active = false;
            http_server = nullptr;
        }
    }
};
