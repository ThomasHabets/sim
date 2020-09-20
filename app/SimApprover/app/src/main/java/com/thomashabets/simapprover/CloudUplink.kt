package com.thomashabets.simapprover;

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.util.Log
import androidx.localbroadcastmanager.content.LocalBroadcastManager
import com.thomashabets.sim.SimProto

// References:
// https://medium.com/@hiten.sahai/secrets-of-firebase-cloud-messaging-android-with-app-in-foreground-and-background-bfde64d8167b

class CloudUplink constructor(in_main: MainActivity): Uplink {
    val main_ = in_main

    companion object {
        val TAG = "SimLog CloudUplink"
    }

    private val message_receiver_: BroadcastReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent) {
            // Extract data included in the Intent
            val msg = intent.getStringExtra("request")
            if (msg == null) {
                Log.d(MainActivity.TAG, "Got Null message??!")
                return;
            }
            Log.d(MainActivity.TAG,"Message received! ${msg.toString()}")
            try {
                main_.add(msg)
            } catch(e:Exception){
                Log.w(MainActivity.TAG, "Unable to add message received: ${e.toString()}")
            }
        }
    }

    fun init() {
        Log.d(TAG, "init()")
    }

    override fun onResume() {
        LocalBroadcastManager.getInstance(main_)
            .registerReceiver(
                message_receiver_,
                IntentFilter("requests")
            )
    }
    override fun onPause() {
        LocalBroadcastManager.getInstance(main_)
            .unregisterReceiver(message_receiver_)
    }

    override fun start() {
        onResume()
    }
    override fun stop() {
    }
    override fun reply(resp: SimProto.ApproveResponse) {
        main_.resetUI()
        throw Exception("not implemented")
    }
    override fun poll(): SimProto.ApproveRequest {
        throw Exception("oh no")
    }
}
