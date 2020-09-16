package com.thomashabets.simapprover.ui.dashboard

import androidx.lifecycle.LiveData
import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.ViewModel

class DashboardViewModel : ViewModel() {

    private val _text = MutableLiveData<String>().apply {
        value = "Multiple hosts not yet implemented"
    }
    val text: LiveData<String> = _text
}