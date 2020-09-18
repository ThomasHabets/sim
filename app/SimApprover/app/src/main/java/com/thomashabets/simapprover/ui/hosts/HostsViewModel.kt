package com.thomashabets.simapprover.ui.hosts

import androidx.lifecycle.LiveData
import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.ViewModel

class HostsViewModel : ViewModel() {

    private val _text = MutableLiveData<String>().apply {
        value = "Multiple hosts not yet implemented"
    }
    val text: LiveData<String> = _text
}