package com.thomashabets.simapprover.ui.hosts

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.fragment.app.Fragment
import androidx.lifecycle.Observer
import androidx.lifecycle.ViewModelProviders
import com.thomashabets.simapprover.R

class HostsFragment : Fragment() {

    private lateinit var hostsViewModel: HostsViewModel

    override fun onCreateView(
            inflater: LayoutInflater,
            container: ViewGroup?,
            savedInstanceState: Bundle?
    ): View? {
        hostsViewModel =
                ViewModelProviders.of(this).get(HostsViewModel::class.java)
        val root = inflater.inflate(R.layout.fragment_hosts, container, false)
        val textView: TextView = root.findViewById(R.id.text_dashboard)
        hostsViewModel.text.observe(viewLifecycleOwner, Observer {
            textView.text = it
        })
        return root
    }
}