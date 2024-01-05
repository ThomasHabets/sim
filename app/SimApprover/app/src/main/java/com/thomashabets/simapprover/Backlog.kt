package com.thomashabets.simapprover

import com.thomashabets.sim.SimProto
import java.util.*


class Backlog {
    val proto_queue_: Queue<SimProto.ApproveRequest> = LinkedList<SimProto.ApproveRequest>()
    val proto_queue_ids_: MutableSet<String> = hashSetOf()

    @Synchronized fun add(req: SimProto.ApproveRequest): Boolean {
        if (!proto_queue_ids_.contains(req.getId())) {
            proto_queue_.add(req)
            proto_queue_ids_.add(req.getId())
            return true
        }
        return false
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
