#include "plc_rule_state_machine.h"
#include "plc_rule_eval.h"   /* check_trigger_edge(), compare_ok(), guard_ok() giả định có sẵn */
#include "plc_rule_action.h" /* execute_action() giả định có sẵn */
#include "plc_tag.h"

/*
 * Máy trạng thái CHẠY LẠI TỪ ĐẦU MỖI VÒNG QUÉT cho các bước Trigger/
 * Compare/Guard (chúng không có khái niệm "kéo dài qua nhiều vòng
 * quét" -- Trigger là 1 sự kiện tức thời, Compare/Guard là phép so
 * sánh tại 1 thời điểm), NHƯNG DWELLING là trạng thái DUY NHẤT thực sự
 * "đứng lại" xuyên suốt nhiều lần gọi hàm này, đúng như RuleRuntimeSM
 * đã lưu rt->state = RULE_STATE_DWELLING giữa 2 lần gọi.
 *
 * Điều này giữ đúng ngữ nghĩa: dwell phải đo trên MỨC hiện tại (qua
 * compare_ok), không phải trên kết quả edge-detect một lần (xem ghi
 * chú lịch sử trong dwell_ok() ở bản if/else trước đó) -- nếu không,
 * dwell không bao giờ đạt cho trigger dạng edge.
 */
bool rule_state_machine_step(Rule *rule, RuleRuntimeSM *rt, uint32_t now_ms)
{
    if (!rule->enabled) {
        rt->state = RULE_STATE_IDLE;
        return false;
    }

    TriggerType trigger_type = (TriggerType)rule->trigger_type;
    bool        is_edge_type = (trigger_type == TRG_ON_RISE  ||
                                 trigger_type == TRG_ON_FALL  ||
                                 trigger_type == TRG_ON_CHANGE);
    int32_t     current       = tag_read(rule->trigger_tag);

    switch (rt->state) {

    case RULE_STATE_IDLE:
    case RULE_STATE_BLOCKED: {
        /* Xét lại từ đầu mỗi vòng quét khi không đang dwell dở dang. */
        bool trigger_met;
        if (is_edge_type) {
            trigger_met = check_trigger_edge(trigger_type, rt->prev_value, current);
        } else {
            /* TRG_TIME_WINDOW / TRG_INTERVAL: xử lý tương tự bản cũ,
               rút gọn ở đây vì không phải trọng tâm câu hỏi. */
            trigger_met = trigger_timing_ok(trigger_type, now_ms, 0,
                                             rule->threshold_lo, rule->threshold_hi,
                                             rule->for_ms, rt->last_fire_tick);
        }
        rt->prev_value = current;

        if (!trigger_met) {
            rt->state = RULE_STATE_BLOCKED;
            return false;
        }
        rt->state = RULE_STATE_TRIGGERED;
    }
    /* FALLTHROUGH */

    case RULE_STATE_TRIGGERED: {
        if (rule->compare_op != OP_NONE &&
            !compare_ok((CompareOp)rule->compare_op, current, rule->threshold_lo, rule->threshold_hi)) {
            rt->state = RULE_STATE_BLOCKED;
            return false;
        }
        rt->state = RULE_STATE_COMPARED;
    }
    /* FALLTHROUGH */

    case RULE_STATE_COMPARED: {
        if (is_edge_type && rule->for_ms > 0) {
            rt->dwell_start_tick = now_ms;
            rt->state = RULE_STATE_DWELLING;
            /* Dwell mới bắt đầu: KHÔNG fire ngay, phải chờ vòng quét sau. */
            return false;
        }
        /* Không cần dwell: đi thẳng sang Guard trong cùng vòng quét. */
        rt->state = RULE_STATE_GUARD_CHECK;
    }
    /* FALLTHROUGH */

    case RULE_STATE_DWELLING: {
        /* ĐÂY LÀ TRẠNG THÁI DUY NHẤT "SỐNG" QUA NHIỀU LẦN GỌI HÀM.
         * Mỗi vòng quét, kiểm tra lại MỨC hiện tại còn giữ đúng ý
         * nghĩa của trigger hay không.
         *
         * BUG ĐÃ SỬA: kiểm tra ban đầu chỉ dựa vào compare_ok(), nhưng
         * compare_op thường là OP_NONE cho các rule dạng edge (ví dụ
         * ON_FALL không kèm ngưỡng nào) -- "OP_NONE => luôn coi là còn
         * giữ mức" khiến dwell không bao giờ bị huỷ dù tín hiệu gốc đã
         * đổi ngược lại (máy chạy lại giữa chừng vẫn không huỷ dwell).
         * Với is_edge_type, "mức còn giữ" phải suy trực tiếp từ chính
         * current so với hướng của trigger_type (ON_FALL => còn giữ
         * nghĩa là current vẫn ==0; ON_RISE => current vẫn !=0), rồi
         * MỚI áp thêm compare_ok() làm điều kiện phụ nếu rule có khai
         * báo compare_op.
         */
        bool level_still_holds;
        if (is_edge_type) {
            bool trigger_level_holds =
                (trigger_type == TRG_ON_FALL)  ? (current == 0) :
                (trigger_type == TRG_ON_RISE)  ? (current != 0) :
                                                  true; /* ON_CHANGE: không có "mức" cố định để giữ */
            level_still_holds = trigger_level_holds &&
                ((rule->compare_op == OP_NONE) ||
                 compare_ok((CompareOp)rule->compare_op, current, rule->threshold_lo, rule->threshold_hi));
        } else {
            level_still_holds = true;
        }

        if (!level_still_holds) {
            /* Điều kiện bị gián đoạn giữa chừng -> huỷ dwell, về BLOCKED.
             * BUG ĐÃ SỬA: prev_value phải được cập nhật ở MỌI nhánh
             * thoát ra khỏi hàm, không chỉ ở case IDLE/BLOCKED -- nếu
             * không, lần gọi kế tiếp sẽ so sánh current với 1 prev_value
             * đã lỗi thời (từ trước khi dwell bắt đầu), làm mất đúng
             * edge mới ngay sau khi huỷ dwell. */
            rt->prev_value       = current;
            rt->dwell_start_tick = DWELL_NOT_STARTED;
            rt->state = RULE_STATE_BLOCKED;
            return false;
        }
        if (now_ms - rt->dwell_start_tick < rule->for_ms) {
            /* Chưa đủ thời gian, GIỮ NGUYÊN state = DWELLING, chờ vòng sau. */
            rt->prev_value = current;
            return false;
        }
        /* Đủ thời gian: chuyển tiếp sang Guard NGAY TRONG vòng quét này. */
        rt->state = RULE_STATE_GUARD_CHECK;
    }
    /* FALLTHROUGH */

    case RULE_STATE_GUARD_CHECK: {
        uint16_t guard_idx = rule->guard_tag & 0x7FFF;
        bool     negate    = (rule->guard_tag & 0x8000) != 0;
        bool     guard_open = (guard_idx == TAG_NONE) ||
                               (negate ? (tag_read(guard_idx) == 0) : (tag_read(guard_idx) != 0));

        if (!guard_open) {
            rt->state = RULE_STATE_BLOCKED;
            return false;
        }
        rt->state = RULE_STATE_FIRE;
    }
    /* FALLTHROUGH */

    case RULE_STATE_FIRE: {
        execute_action(rule);
        rt->last_fire_tick   = now_ms;
        rt->dwell_start_tick = DWELL_NOT_STARTED;
        rt->state = RULE_STATE_IDLE;   
        return true;
    }

    default:
        rt->state = RULE_STATE_IDLE;
        return false;
    }

    return false;
}