package com.thomashabets.simapprover

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.os.Bundle
import androidx.preference.PreferenceManager
import android.util.Log
import android.util.Xml
import android.view.Menu
import android.view.MenuItem
import android.widget.Toast
import com.google.android.material.bottomnavigation.BottomNavigationView
import androidx.appcompat.app.AppCompatActivity
import androidx.navigation.findNavController
import androidx.navigation.ui.AppBarConfiguration
import androidx.navigation.ui.setupActionBarWithNavController
import androidx.navigation.ui.setupWithNavController
import com.google.android.gms.common.api.ApiException
import com.google.android.gms.tasks.OnCompleteListener
import com.google.android.gms.tasks.Task
import com.google.android.material.snackbar.Snackbar
import com.google.firebase.FirebaseApp
import com.google.firebase.iid.FirebaseInstanceId
import com.google.firebase.messaging.FirebaseMessaging
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
import kotlinx.coroutines.*
import java.io.FileNotFoundException
import kotlinx.coroutines.channels.ClosedReceiveChannelException
import kotlinx.coroutines.channels.ReceiveChannel
import java.util.*


class Backlog {
    val proto_queue_: Queue<SimProto.ApproveRequest> = LinkedList<SimProto.ApproveRequest>()
    val proto_queue_ids_: MutableSet<String> = hashSetOf()

    @Synchronized fun add(req: SimProto.ApproveRequest) {
        if (!proto_queue_ids_.contains(req.getId())) {
            proto_queue_.add(req)
            proto_queue_ids_.add(req.getId())
        }
    }

    @Synchronized fun head(): SimProto.ApproveRequest? {
        return proto_queue_.peek()
    }

    @Synchronized fun pop(): SimProto.ApproveRequest? {
        val tmp = proto_queue_.poll()
        if (tmp != null) {
            proto_queue_ids_.remove(tmp.getId())
        }
        return tmp
    }
    @Synchronized fun clear() {
        proto_queue_ids_.clear()
        proto_queue_.clear()
    }
}

class MainActivity : AppCompatActivity() {
    companion object {
        val TAG = "SimLog"
    }

    // Default settings.
    val defaultBasePath = "/sim"
    val defaultBaseHost = "shell.example.com"
    val default_pin_ = "some very secret password"

    // Loaded settings.
    var baseHost = ""
    var basePath = ""
    var pin_ = ""

    // Approval backlog
    val backlog_ = Backlog()

    val user_agent_ = "SimApprover 0.01"
    var poller_generation_ = 0

    var websocket_: DefaultWebSocketSession? = null

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        // Inflate the menu; this adds items to the action bar if it is present.
        menuInflater.inflate(R.menu.dropdown, menu)
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        val id = item.getItemId()
        if (id == R.id.navigation_settings) {
            startActivity(Intent(this@MainActivity, SettingsActivity::class.java))
            return true
        }
        if (id == R.id.copy_device_token) {
            FirebaseInstanceId.getInstance().instanceId
                .addOnCompleteListener(OnCompleteListener { task ->
                    if (!task.isSuccessful) {
                        Log.w(TAG, "getInstanceId failed", task.exception)
                        return@OnCompleteListener
                    }
                    // Get new Instance ID token
                    val token = task.result?.token
                    Log.d(TAG,"FCM my token is ${token}")

                    val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                    val clip: ClipData = ClipData.newPlainText("simple text", token)
                    clipboard.setPrimaryClip(clip)

                    // Log and toast
                    val msg = getString(R.string.msg_token_fmt, token)
                    Log.d(TAG, "FCM getstring something ${msg}")
                    //Toast.makeText(baseContext, msg, Toast.LENGTH_SHORT).show()
                    Toast.makeText(baseContext, "device token copied", Toast.LENGTH_SHORT).show()
                })
        }
        return super.onOptionsItemSelected(item)
    }

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

        //supportActionBar.setDisplayOptions()
        //actionBar.setTitle("bice")

        // Get/set settings.
        val sharedPreferences = PreferenceManager.getDefaultSharedPreferences(this)
        baseHost = sharedPreferences.getString("base_host", defaultBaseHost)!!
        basePath = sharedPreferences.getString("base_path", defaultBasePath)!!
        pin_ = sharedPreferences.getString("pin", default_pin_)!!
        poll_switch.setChecked(sharedPreferences.getBoolean("poll", true))
        sharedPreferences.edit().putString("base_host", baseHost).apply()
        sharedPreferences.edit().putString("base_path", basePath).apply()
        sharedPreferences.edit().putString("pin", pin_).apply()

        setup_fcm()

        // Start polling.
        if (poll_switch.isChecked()) {
            start_poll_stream()
        }

        // Set up buttons handlers.
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
            sharedPreferences.edit().putBoolean("poll", checked).apply()
            if (checked) {
                start_poll_stream()
            } else {
                stop_poll_stream()
            }
        }
    }

    fun setup_fcm() {
        val topic = getString(R.string.topic_everyone)
            Log.d(TAG, "Subscribing to topic ${topic}")
            FirebaseMessaging.getInstance().subscribeToTopic(topic)
                .addOnCompleteListener { task ->
                    var msg = getString(R.string.msg_subscribed)
                    if (!task.isSuccessful) {
                        msg = getString(R.string.msg_subscribe_failed)
                    }
                    Log.d(TAG, msg)
                    //Toast.makeText(baseContext, msg, Toast.LENGTH_SHORT).show()
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
        backlog_.clear()
        poller_generation_++
        if (websocket_ != null) {
            Log.d(TAG,"Cancelling connection")
            websocket_!!.cancel()
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
                }
                var done = false
                while (!done) {
                    val frame = incoming.receive()
                    when (frame) {
                        is Frame.Text -> {
                            val id = frame.readText()
                            synchronized(this@MainActivity) {
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
                    backlog_.add(proto)
                    drawRequest(backlog_.head())
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
    private fun replyButton(approve: Boolean) {
        val proto = backlog_.pop()
        if (proto != null) {
            val resp = SimProto.ApproveResponse.newBuilder()
            resp.setId(proto.getId())
            resp.setApproved(approve)
            Thread {
                try {
                    reply(resp.build())
                } catch (e: FileNotFoundException) {
                    showError("Command no longer exists")
                    resetUI()
                    drawRequest(backlog_.head())
                } catch (e: Exception) {
                    showError("Failed to reply: " + e.toString())
                }
            }.start()
        }
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
            drawRequest(backlog_.head())
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