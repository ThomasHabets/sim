package com.thomashabets.simapprover

import android.content.*
import android.os.Bundle
import android.util.Log
import android.view.Menu
import android.view.MenuItem
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.navigation.findNavController
import androidx.navigation.ui.AppBarConfiguration
import androidx.navigation.ui.setupActionBarWithNavController
import androidx.navigation.ui.setupWithNavController
import androidx.preference.PreferenceManager
import com.google.android.gms.tasks.OnCompleteListener
import com.google.android.material.bottomnavigation.BottomNavigationView
import com.google.android.material.snackbar.Snackbar
import com.google.firebase.iid.FirebaseInstanceId
import com.google.firebase.messaging.FirebaseMessaging
import com.google.protobuf.ByteString
import com.thomashabets.sim.SimProto
import kotlinx.android.synthetic.main.activity_main.*
import java.io.BufferedReader
import java.io.FileNotFoundException
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL
import java.util.*


public interface Uplink {
    fun poll(): SimProto.ApproveRequest
    fun start()
    fun stop()
    fun onPause()
    fun onResume()
    fun reply(re: SimProto.ApproveResponse)
    fun init()
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
    var base_host_ = ""
    var base_path_ = ""
    var pin_ = ""

    // Approval backlog
    val backlog_ = Backlog()

    var uplink_: Uplink = CloudUplink(this)
    //val uplink_ = HTTPSUplink(this)

    val user_agent_ = "SimApprover 0.01"


    fun get_base_host(): String{return base_host_}
    fun get_base_path(): String{return base_path_}
    fun get_pin():String{return pin_}
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

        uplink_.init()
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
        base_host_ = sharedPreferences.getString("base_host", defaultBaseHost)!!
        base_path_ = sharedPreferences.getString("base_path", defaultBasePath)!!
        pin_ = sharedPreferences.getString("pin", default_pin_)!!
        poll_switch.setChecked(sharedPreferences.getBoolean("poll", true))
        sharedPreferences.edit().putString("base_host", base_host_).apply()
        sharedPreferences.edit().putString("base_path", base_path_).apply()
        sharedPreferences.edit().putString("pin", pin_).apply()

        onResume()
        setup_fcm()

        // Start polling.
        if (poll_switch.isChecked()) {
            uplink_.start()
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
                uplink_.start()
            } else {
                uplink_.stop()
            }
        }
    }

    fun set_status(s: String) {
        runOnUiThread {
            status_text.setText(s)
        }
    }
    override fun onResume() {
        super.onResume()
        uplink_.onResume()
    }

    override fun onPause() {
        super.onPause()
        uplink_.onPause()
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


    fun add(str: String) {
        add(str.toByteArray())
    }
    fun add(bytes: ByteArray) {
        val b2 = ByteString.copyFrom(bytes)
        val proto = SimProto.ApproveRequest.parseFrom(b2!!)
        backlog_.add(proto)
        drawRequest(backlog_.head())
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
                    uplink_.reply(resp.build())
                    drawRequest(backlog_.head())
                } catch (e: FileNotFoundException) {
                    show_error("Command no longer exists")
                    resetUI()
                    drawRequest(backlog_.head())
                } catch (e: Exception) {
                    show_error("Failed to reply: " + e.toString())
                }
            }.start()
        }
    }

    // Show error bar.
    fun show_error(e: String) {
        Log.w(TAG, "Snackbar error: ${e}")
        runOnUiThread {
            Snackbar.make(
                findViewById(R.id.container),
                e,
                Snackbar.LENGTH_LONG
            ).show()
        }
    }

    fun getUserAgent(): String {
        return "$user_agent_ ${System.getProperty("http.agent")}"
    }

    // Clear the UI of any command request.
    fun resetUI() {
        runOnUiThread {
            status_text.setText("Status: waiting for next approval")
            approve_button.setEnabled(false)
            reject_button.setEnabled(false)
            protobuf.setText("")
        }
    }
}