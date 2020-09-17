package com.thomashabets.simapprover

import android.content.Intent
import android.os.Bundle
import android.util.Log
import android.util.Xml
import com.google.android.material.bottomnavigation.BottomNavigationView
import androidx.appcompat.app.AppCompatActivity
import androidx.navigation.findNavController
import androidx.navigation.ui.AppBarConfiguration
import androidx.navigation.ui.setupActionBarWithNavController
import androidx.navigation.ui.setupWithNavController
import com.google.android.gms.common.api.ApiException
import com.google.android.gms.tasks.Task
import com.google.android.material.snackbar.Snackbar
import com.google.firebase.FirebaseApp
import com.google.protobuf.ByteString
import kotlinx.android.synthetic.main.activity_main.*
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import com.thomashabets.sim.SimProto
import io.ktor.client.HttpClient
import io.ktor.client.features.websocket.WebSockets
import io.ktor.client.features.websocket.ws
import io.ktor.client.features.websocket.wss
import io.ktor.client.request.header
import io.ktor.http.HttpMethod
import io.ktor.http.cio.websocket.DefaultWebSocketSession
import io.ktor.http.cio.websocket.Frame
import io.ktor.http.cio.websocket.readText
import kotlinx.android.synthetic.main.fragment_home.*
import java.io.FileNotFoundException
import kotlinx.coroutines.GlobalScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.ClosedReceiveChannelException
import kotlinx.coroutines.channels.ReceiveChannel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.util.*


class MainActivity : AppCompatActivity() {
    companion object {
        val TAG = "SimLog"
    }

    var currentProto: SimProto.ApproveRequest? = null
    val baseHost = "shell.example.com"
    val basePath = "/sim"
    val pin_ = "some secret password here"
    var proto_queue_: Queue<SimProto.ApproveRequest> = LinkedList<SimProto.ApproveRequest>()
    val user_agent_ = "SimApprover 0.01"
    var stream_: ReceiveChannel<Frame>? = null
    var poller_generation_ = 0
    var websocket_: DefaultWebSocketSession? = null
    var client_: HttpClient? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        val navView: BottomNavigationView = findViewById(R.id.nav_view)
        val navController = findNavController(R.id.nav_host_fragment)

        // Passing each menu ID as a set of Ids because each
        // menu should be considered as top level destinations.
        val appBarConfiguration = AppBarConfiguration(
            setOf(
                R.id.navigation_home, R.id.navigation_dashboard, R.id.navigation_notifications
            )
        )
        setupActionBarWithNavController(navController, appBarConfiguration)
        navView.setupWithNavController(navController)

        if (poll_switch.isChecked()) {
            start_poll_stream()
        }

        reject_button.setOnClickListener { _ ->
            Log.d(TAG, "Rejecting")
            replyButton(false)
        }
        approve_button.setOnClickListener { _ ->
            Log.d(TAG, "Approving")
            replyButton(true)

        }
        poll_switch.setOnCheckedChangeListener {_, checked ->
            Log.d(TAG, "Changed poll switch to " + checked.toString())
            if (checked) {
                start_poll_stream()
            } else {
                stop_poll_stream()
            }
        }
    }

    // Start the polling stream.
    @Synchronized private fun start_poll_stream() {
        Log.d(TAG, "poll button pressed")
        poller_generation_++
        val my_generation = poller_generation_
        runOnUiThread{status_text.setText("Status: poller starting")}
        resetUI()
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
                            runOnUiThread { status_text.setText("Status: connected and waiting") }
                            poll_stream(my_generation, client)
                        } catch (e: ClosedReceiveChannelException) {
                            Log.d(TAG, "websocket closed by server. Restarting…")
                            runOnUiThread { status_text.setText("Status: poller disconnected") }
                        } catch (e: Exception) {
                            Log.d(TAG, "Exception in poller, restarting: " + e.toString())
                            runOnUiThread { status_text.setText("Status: poller disconnected") }
                            delay(10000) // TODO: switch to exponential backoff
                        }
                    }
                }
            } catch(e: Exception){
                Log.d(TAG, "start_poll_stream exception thingy happened")
                throw e
            }
        }.start()
    }

    // Stop polling stream.
    @Synchronized private fun stop_poll_stream() {
        Log.d(TAG, "Stopping poll stream")
        poller_generation_++
        currentProto = null
        if (websocket_ != null) {
            Log.d(TAG,"Cancelling connection")
            stream_!!.cancel()
            websocket_!!.run {
                terminate()
            }
        }
        Thread{runOnUiThread{status_text.setText("Status: idle")}}.start()
        resetUI()
    }

    // Stream IDs to approve.
    suspend private fun poll_stream(my_generation: Int, client: HttpClient) {
        Log.d(TAG, "Websocket starting up")
        try {
            client.wss(
                method = HttpMethod.Get,
                host = baseHost,
                //port = 8080,
                path = basePath + "/stream",
                request = {
                    header("x-sim-PIN", pin_)
                    header("user-agent", getUserAgent())
                }
            ) {
                synchronized(this@MainActivity) {
                    websocket_ = this
                    stream_ = incoming
                    client_ = client
                }
                var done = false
                while (!done) {
                    val frame = incoming.receive()
                    when (frame) {
                        is Frame.Text -> {
                            val id = frame.readText()
                            var done = false
                            synchronized(this@MainActivity) {
                                if (poller_generation_ != my_generation) {
                                    // User cancelled.
                                    Log.d(TAG, "Poller shutting down")
                                    outgoing.close()
                                    incoming.cancel()
                                    websocket_!!.terminate()
                                    client.close()

                                    done = true
                                }
                            }
                            if (!done) {
                                Log.d(TAG, "Got streamed id: " + id)

                                Thread {
                                    try {
                                        poll_stream_got_one(id)
                                    } catch (e: Exception) {
                                        showError("Failed to get ID " + id + ": " + e.toString())
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
            val url = URL("https://${baseHost}${basePath}/get/" + id)
            with(url.openConnection() as HttpURLConnection) {
                try {
                    setRequestProperty("x-sim-pin", pin_)
                    setRequestProperty("user-agent", getUserAgent())
                    val bytes = inputStream.readBytes()
                    val b2 = ByteString.copyFrom(bytes)
                    val proto = SimProto.ApproveRequest.parseFrom(b2!!)
                    synchronized(this) {
                        if (currentProto == null) {
                            currentProto = proto
                            drawRequest(proto)
                        } else {
                            proto_queue_.add(proto)
                        }
                    }
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

    private fun drawRequest(req: SimProto.ApproveRequest?){
        if (req == null) {
            return
        }
        runOnUiThread {
            status_text.setText("Status: awaiting user action")
            protobuf.setText(req.toString())
            approve_button.setEnabled(true)
            reject_button.setEnabled(true)
        }
    }

    // Button handler for both approve and reject.
    @Synchronized private fun replyButton(approve: Boolean) {
        if (currentProto == null){
            return
        }
        val resp = SimProto.ApproveResponse.newBuilder()
        resp.setId(currentProto!!.getId())
        resp.setApproved(approve)
        Thread {
            try {
                reply(resp.build())
            } catch (e: FileNotFoundException) {
                showError("Command no longer exists")
                resetUI()
                set_next_proto()
            } catch (e: Exception) {
                showError("Failed to reply: " + e.toString())
            }
        }.start()
    }

    @Synchronized private fun set_next_proto() {
        currentProto = proto_queue_.poll()
        drawRequest(currentProto)
    }

    // Show error bar.
    private fun showError(e: String) {
        Log.w(TAG, "Snackbar error: ${e}")
        runOnUiThread {
            Snackbar.make(
                findViewById(R.id.container),
                e,
                Snackbar.LENGTH_LONG
            ).show()
        }
    }

    private fun getUserAgent(): String {
        return "$user_agent_ ${System.getProperty("http.agent")}"
    }

    // Send reply.
    private fun reply(resp: SimProto.ApproveResponse) {
        val mURL = URL("https://${baseHost}${basePath}/approve/"+resp.getId())
        val serialized = resp.toByteArray()
        Log.i(TAG, "Replying: " + resp.toString())

        with(mURL.openConnection() as HttpURLConnection) {
            requestMethod = "POST"
            setRequestProperty("x-sim-pin", pin_)
            setRequestProperty("user-agent", getUserAgent())
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
            resetUI()
            set_next_proto()
        }
    }

    // Clear the UI of any command request.
    private fun resetUI() {
        runOnUiThread {
            status_text.setText("Status: waiting for next approval")
            approve_button.setEnabled(false)
            reject_button.setEnabled(false)
            protobuf.setText("")
        }
    }
}