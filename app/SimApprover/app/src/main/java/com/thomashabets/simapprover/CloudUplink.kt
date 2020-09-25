package com.thomashabets.simapprover;

// import com.google.crypto.tink.aead.subtle.AesGcmFactory
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.util.Log
import androidx.localbroadcastmanager.content.LocalBroadcastManager
import com.google.crypto.tink.config.TinkConfig
import com.google.crypto.tink.subtle.AesGcmJce
import com.google.crypto.tink.subtle.Random
import com.google.gson.Gson
import com.thomashabets.sim.SimProto
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.*

//import com.google.crypto.tink.aead.subtle.AesGcmFactory


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
                Log.d(TAG, "Got Null message??!")
                return;
            }

            // Decode.
            val dig = MessageDigest.getInstance("SHA-256")
            val key = dig.digest(main_.get_pin().toByteArray())
            val aes = AesGcmJce(key)
            val decoded_msg = Base64.getDecoder().decode(msg);

            try {
                // Decrypt.
                val plain = aes.decrypt(decoded_msg, null)
                Log.d(TAG,"Message received and decrypted!")

                // Pass to MainActivity.
                main_.add(plain)
            } catch(e:Exception){
                Log.w(TAG, "Unable to decrypt/add message received: ${e.toString()}")
            }
        }
    }

    override fun init() {
        Log.d(TAG, "init()")
        TinkConfig.register()

        // Test encrypt/decrypt.
        val keySize=32
        val aad = byteArrayOf(1, 2, 3)
        val key =
            Random.randBytes(keySize)
        val gcm = AesGcmJce(key)
        for (messageSize in 0..74) {
            val message =
                Random.randBytes(messageSize)
            val ciphertext = gcm.encrypt(message, aad)
            val decrypted = gcm.decrypt(ciphertext, aad)
        }
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

        // Encrypt.
        val dig = MessageDigest.getInstance("SHA-256")
        val key = dig.digest(main_.get_pin().toByteArray())
        val aes = AesGcmJce(key)
        val encrypted = aes.encrypt(resp.toByteArray(), null)
        val encoded_msg = Base64.getEncoder().encode(encrypted);

        // Wrap in JSON.
        val rr = ReplyStruct(resp.getId(), encoded_msg)
        val json = Gson().toJson(rr)

        // Send to the cloud.
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
    }
    override fun poll(): SimProto.ApproveRequest {
        throw Exception("polling not implemented, and why would it be?")
    }
}
