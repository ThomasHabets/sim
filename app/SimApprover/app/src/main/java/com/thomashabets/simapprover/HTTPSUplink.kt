package com.thomashabets.simapprover

import android.util.Log
import com.thomashabets.sim.SimProto
import io.ktor.client.HttpClient
import io.ktor.client.features.websocket.WebSockets
import io.ktor.client.features.websocket.wss
import io.ktor.client.request.header
import io.ktor.http.HttpMethod
import io.ktor.http.cio.websocket.DefaultWebSocketSession
import io.ktor.http.cio.websocket.Frame
import io.ktor.http.cio.websocket.readText
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.ClosedReceiveChannelException
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL

class HTTPSUplink(in_main: MainActivity): Uplink {
    val main_ = in_main;
    var poller_generation_ = 0
    var websocket_: DefaultWebSocketSession? = null

    companion object {
        val TAG = "SimLog HTTPSUplink"
    }

    fun init() {
        Log.d(TAG, "init()")
    }
    override fun poll(): SimProto.ApproveRequest {
        throw Exception("not implemented")
    }
    override fun onResume() {    }
    override fun onPause(){}
    override fun start()
    {
        Log.d(TAG, "poll button pressed")
        poller_generation_++
        val my_generation = poller_generation_
        main_.set_status("Status: poller starting")
        main_.resetUI()
        Thread {
            try {
                Log.d(TAG, "ws thread started")
                val client = HttpClient {
                    install(WebSockets) {
                        //pingPeriod = Duration.ofMinutes(1)
                    }
                }
                GlobalScope.launch(Dispatchers.Main) {
                    var done = false
                    while (!done) {
                        synchronized(this) {
                            if (my_generation != poller_generation_) {
                                done = true
                            }
                        }
                        try {
                            main_.set_status("Status: connected and waiting")
                            poll_stream(my_generation, client)
                        } catch (e: ClosedReceiveChannelException) {
                            Log.d(TAG, "websocket closed by server. Restarting…")
                            main_.set_status("Status: poller disconnected")
                        } catch (e: Exception) {
                            Log.d(
                                TAG,
                                "Exception in poller, restarting: " + e.toString()
                            )
                            main_.set_status("Status: poller disconnected")
                            delay(10000) // TODO: switch to exponential backoff
                        }
                    }
                }
            } catch (e: Exception) {
                Log.d(TAG, "start_poll_stream exception thingy happened")
                throw e
            }
        }.start()
    }

    // Stream IDs to approve.
    suspend private fun poll_stream(my_generation: Int, client: HttpClient) {
        Log.d(TAG, "Websocket starting up")
        try {
            client.wss(
                method = HttpMethod.Get,
                host = main_.get_base_host(),
                //port = 8080,
                path = "${main_.get_base_path()}/stream",
                request = {
                    header("x-sim-PIN", main_.get_pin())
                    header("user-agent", main_.getUserAgent())
                }
            ) {
                synchronized(this@HTTPSUplink) {
                    websocket_ = this
                }
                var done = false
                while (!done) {
                    val frame = incoming.receive()
                    when (frame) {
                        is Frame.Text -> {
                            val id = frame.readText()
                            synchronized(this@HTTPSUplink) {
                                if (poller_generation_ != my_generation) {
                                    // User cancelled.
                                    Log.d(TAG, "Poller shutting down")
                                    done = true
                                }
                            }
                            if (!done) {
                                Log.d(TAG, "Got streamed id: " + id)

                                Thread {
                                    try {
                                        poll_stream_got_one(id)
                                    } catch (e: Exception) {
                                        main_.show_error("Failed to get ID " + id + ": " + e.toString())
                                    }
                                }.start()
                            }
                        }
                    }
                }
            }
        }catch (e:Exception){
            Log.d(TAG,"The suspended thing had an exception: " + e.toString())
            throw e
        }
    }
    // An ID has been retrieved. Retrieve the rest of the data.
    private fun poll_stream_got_one(id: String) {
        Log.d(TAG, "getting that ID")
        try {
            val url = URL("https://${main_.get_base_host()}${main_.get_base_path()}/get/" + id)
            with(url.openConnection() as HttpURLConnection) {
                try {
                    setRequestProperty("x-sim-pin", main_.get_pin())
                    setRequestProperty("user-agent", main_.getUserAgent())
                    main_.add(inputStream.readBytes())
                } catch (e:Exception){
                    Log.d(TAG,"exception inside in poll_streaM_got_one")
                    throw e
                }
            }
        } catch(e:Exception){
            Log.d(TAG,"Exception in poll_stream_got_one")
            throw e
        }
    }

    // Send reply.
    override fun reply(resp: SimProto.ApproveResponse) {
        val mURL = URL("https://${main_.get_base_host()}${main_.get_base_path()}/approve/"+resp.getId())
        val serialized = resp.toByteArray()
        Log.i(TAG, "Replying: " + resp.toString())

        with(mURL.openConnection() as HttpURLConnection) {
            requestMethod = "POST"
            setRequestProperty("x-sim-pin", main_.get_pin())
            setRequestProperty("user-agent", main_.getUserAgent())
            getOutputStream().write(serialized)
            getOutputStream().close()

            Log.d(TAG, "URL : $url")
            Log.d(TAG, "Response Code : $responseCode")

            BufferedReader(InputStreamReader(inputStream)).use {
                val response = StringBuffer()

                var inputLine = it.readLine()
                while (inputLine != null) {
                    response.append(inputLine)
                    inputLine = it.readLine()
                }
                Log.i(TAG, "Response : $response")
            }
            main_.resetUI()
        }
    }


    override fun stop() {
        Log.d(TAG, "Stopping poll stream")
        main_.backlog_.clear()
        poller_generation_++
        if (websocket_ != null) {
            Log.d(TAG,"Cancelling connection")
            websocket_!!.cancel()
        }
        main_.set_status("Status: idle")
        main_.resetUI()
    }
}