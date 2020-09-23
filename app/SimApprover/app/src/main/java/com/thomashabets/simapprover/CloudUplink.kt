package com.thomashabets.simapprover;

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.util.Log
import androidx.localbroadcastmanager.content.LocalBroadcastManager
import com.thomashabets.sim.SimProto

import com.google.gson.Gson
import com.google.gson.GsonBuilder
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL

// References:
// https://medium.com/@hiten.sahai/secrets-of-firebase-cloud-messaging-android-with-app-in-foreground-and-background-bfde64d8167b

class ReplyStruct (
    val id: String,
    val content: ByteArray){
}

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
        Log.i(TAG, "Replying…")
        val rr = ReplyStruct(resp.getId(), resp.toByteArray())

        val json = Gson().toJson(rr)
        val mURL = URL("https://europe-west2-simapprover.cloudfunctions.net/reply")
        with(mURL.openConnection() as HttpURLConnection) {
            requestMethod = "POST"
            setRequestProperty("user-agent", main_.getUserAgent())
            getOutputStream().write(json.toByteArray())
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

        main_.resetUI()
    }
    override fun poll(): SimProto.ApproveRequest {
        throw Exception("polling not implemented, and why would it be?")
    }
}
