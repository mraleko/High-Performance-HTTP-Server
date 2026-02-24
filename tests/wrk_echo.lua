wrk.method = "POST"
wrk.body = "hello from wrk"
wrk.headers["Content-Length"] = tostring(#wrk.body)
