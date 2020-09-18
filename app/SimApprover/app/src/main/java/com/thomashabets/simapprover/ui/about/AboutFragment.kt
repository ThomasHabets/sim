package com.thomashabets.simapprover.ui.about

import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.fragment.app.Fragment
import androidx.lifecycle.Observer
import androidx.lifecycle.ViewModelProviders
import com.thomashabets.simapprover.R

class AboutFragment : Fragment() {
    companion object {
        val TAG = "SimLog"
    }

    private lateinit var aboutViewModel: AboutViewModel

    override fun onCreateView(
            inflater: LayoutInflater,
            container: ViewGroup?,
            savedInstanceState: Bundle?
    ): View? {
        aboutViewModel =
                ViewModelProviders.of(this).get(AboutViewModel::class.java)
        val root = inflater.inflate(R.layout.fragment_about, container, false)
        val textView: TextView = root.findViewById(R.id.text_notifications)
        aboutViewModel.text.observe(viewLifecycleOwner, Observer {
            textView.text = it
        })

        Log.d(TAG, "Switched to 'About'")


        return root
    }
    override fun onDestroyView(){
        super.onDestroyView()

        Log.d(TAG,"Destroying 'about'")
    }
}