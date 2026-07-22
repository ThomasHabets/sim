package com.thomashabets.simapprover

import android.Manifest
import android.content.*
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.localbroadcastmanager.content.LocalBroadcastManager
import androidx.navigation.findNavController
import androidx.navigation.ui.AppBarConfiguration
import androidx.navigation.ui.setupActionBarWithNavController
import androidx.navigation.ui.setupWithNavController
import androidx.preference.PreferenceManager
import com.google.android.material.bottomnavigation.BottomNavigationView
import com.google.android.material.snackbar.Snackbar
import com.google.firebase.messaging.FirebaseMessaging
import com.google.protobuf.ByteString
import com.thomashabets.sim.SimProto
import com.thomashabets.simapprover.databinding.ActivityMainBinding
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
        private const val NOTIFICATION_PERMISSION_REQUEST_CODE = 1
    }

    private lateinit var binding: ActivityMainBinding

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

    var uplink_: Uplink? = null

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
            FirebaseMessaging.getInstance().token
                .addOnCompleteListener { task ->
                    if (!task.isSuccessful) {
                        Log.w(TAG, "Fetching FCM registration token failed", task.exception)
                        return@addOnCompleteListener
                    }
                    val token = task.result
                    Log.d(TAG,"FCM my token is ${token}")

                    val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                    val clip: ClipData = ClipData.newPlainText("simple text", token)
                    clipboard.setPrimaryClip(clip)

                    // Log and toast
                    val msg = getString(R.string.msg_token_fmt, token)
                    Log.d(TAG, "FCM getstring something ${msg}")
                    //Toast.makeText(baseContext, msg, Toast.LENGTH_SHORT).show()
                    Toast.makeText(baseContext, "device token copied", Toast.LENGTH_SHORT).show()
                }
        }
        return super.onOptionsItemSelected(item)
    }

    @Synchronized
    fun set_uplink(use_cloud: Boolean) {
        if (uplink_ != null ) {
            uplink_!!.stop()
        }
        if (use_cloud) {
            runOnUiThread{
                binding.pollLabel.setVisibility(View.INVISIBLE)
                binding.pollSwitch.setVisibility(View.INVISIBLE)
                binding.statusText.setVisibility(View.INVISIBLE)
            }
            uplink_ = CloudUplink(this)
        } else {
            runOnUiThread{
                binding.pollLabel.setVisibility(View.VISIBLE)
                binding.pollSwitch.setVisibility(View.VISIBLE)
                binding.statusText.setVisibility(View.VISIBLE)
            }
            uplink_ = HTTPSUplink(this)
        }
        uplink_!!.init()
        uplink_!!.start()
    }

    @Synchronized
    fun get_uplink(): Uplink {
        return uplink_!!
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        val navView: BottomNavigationView = binding.navView
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
        base_host_ = sharedPreferences.getString("base_host", defaultBaseHost)!!
        base_path_ = sharedPreferences.getString("base_path", defaultBasePath)!!
        pin_ = sharedPreferences.getString("pin", default_pin_)!!

        val use_cloud = sharedPreferences.getBoolean("use_cloud", false)
        set_uplink(use_cloud)

        binding.pollSwitch.setChecked(sharedPreferences.getBoolean("poll", true))
        sharedPreferences.edit().putString("base_host", base_host_).apply()
        sharedPreferences.edit().putString("base_path", base_path_).apply()
        sharedPreferences.edit().putString("pin", pin_).apply()

        onResume()
        setup_fcm()
        requestNotificationPermission()

        // Start polling.
        if (binding.pollSwitch.isChecked()) {
            get_uplink().start()
        }

        // Set up buttons handlers.
        binding.rejectButton.setOnClickListener { _ ->
            Log.d(TAG, "Rejecting")
            replyButton(false)
        }
        binding.approveButton.setOnClickListener { _ ->
            Log.d(TAG, "Approving")
            replyButton(true)
        }
        binding.pollSwitch.setOnCheckedChangeListener {_, checked ->
            Log.d(TAG, "Changed poll switch to " + checked.toString())
            sharedPreferences.edit().putBoolean("poll", checked).apply()
            if (checked) {
                get_uplink().start()
            } else {
                get_uplink().stop()
            }
        }
    }

    fun set_status(s: String) {
        runOnUiThread {
            binding.statusText.setText(s)
        }
    }
    override fun onResume() {
        super.onResume()
        get_uplink().onResume()
        LocalBroadcastManager.getInstance(this)
            .registerReceiver(
                message_receiver_,
                IntentFilter("settings")
            )
        Log.d(TAG, "Settings were updated")
        val sharedPreferences = PreferenceManager.getDefaultSharedPreferences(this)
        set_uplink(sharedPreferences.getBoolean("use_cloud", false))
    }

    override fun onPause() {
        super.onPause()
        get_uplink().onPause()
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

    private fun requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(
                arrayOf(Manifest.permission.POST_NOTIFICATIONS),
                NOTIFICATION_PERMISSION_REQUEST_CODE
            )
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
            binding.statusText.setText("Status: awaiting user action")
            binding.protobuf.setText(req.toString())
            binding.approveButton.setEnabled(true)
            binding.rejectButton.setEnabled(true)
        }
    }

    private val message_receiver_: BroadcastReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent) {

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
                    get_uplink().reply(resp.build())
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
            binding.statusText.setText("Status: waiting for next approval")
            binding.approveButton.setEnabled(false)
            binding.rejectButton.setEnabled(false)
            binding.protobuf.setText("")
        }
    }
}
