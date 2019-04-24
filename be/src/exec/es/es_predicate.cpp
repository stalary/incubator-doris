// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exec/es/es_predicate.h"

#include <stdint.h>
#include <map>
#include <sstream>
#include <boost/algorithm/string.hpp>
#include <gutil/strings/substitute.h>

#include "common/status.h"
#include "common/logging.h"
#include "exec/es/es_query_builder.h"
#include "exprs/expr.h"
#include "exprs/expr_context.h"
#include "exprs/in_predicate.h"

#include "gen_cpp/PlanNodes_types.h"
#include "olap/olap_common.h"
#include "olap/utils.h"
#include "runtime/client_cache.h"
#include "runtime/runtime_state.h"
#include "runtime/row_batch.h"
#include "runtime/datetime_value.h"
#include "runtime/large_int_value.h"
#include "runtime/string_value.h"
#include "runtime/tuple_row.h"

#include "service/backend_options.h"
#include "util/debug_util.h"
#include "util/runtime_profile.h"

namespace doris {

using namespace std;

std::string ExtLiteral::value_to_string() {
    std::stringstream ss;
    switch (_type) {
        case TYPE_TINYINT:
            ss << std::to_string(get_byte());
            break;
        case TYPE_SMALLINT:
            ss << std::to_string(get_short());
            break;
        case TYPE_INT:
            ss << std::to_string(get_int());
            break;
        case TYPE_BIGINT:
            ss << std::to_string(get_long());
            break;
        case TYPE_FLOAT:
            ss << std::to_string(get_float());
            break;
        case TYPE_DOUBLE:
            ss << std::to_string(get_double());
            break;
        case TYPE_CHAR:
        case TYPE_VARCHAR:
            ss << get_string();
            break;
        case TYPE_DATE:
        case TYPE_DATETIME:
            ss << get_date_string();
            break;
        case TYPE_BOOLEAN:
            ss << std::to_string(get_bool());
            break;
        case TYPE_DECIMAL:
            ss << get_decimal_string();
            break;
        case TYPE_DECIMALV2:
            ss << get_decimalv2_string();
            break;
        case TYPE_LARGEINT:
            ss << get_largeint_string();
            break;
        default:
            DCHECK(false);
            break;
    }
    return ss.str();
}

ExtLiteral::~ExtLiteral(){
}

int8_t ExtLiteral::get_byte() {
    DCHECK(_type == TYPE_TINYINT);
    return *(reinterpret_cast<int8_t*>(_value));
}

int16_t ExtLiteral::get_short() {
    DCHECK(_type == TYPE_SMALLINT);
    return *(reinterpret_cast<int16_t*>(_value));
}

int32_t ExtLiteral::get_int() {
    DCHECK(_type == TYPE_INT);
    return *(reinterpret_cast<int32_t*>(_value));
}

int64_t ExtLiteral::get_long() {
    DCHECK(_type == TYPE_BIGINT);
    return *(reinterpret_cast<int64_t*>(_value));
}

float ExtLiteral::get_float() {
    DCHECK(_type == TYPE_FLOAT);
    return *(reinterpret_cast<float*>(_value));
}

double ExtLiteral::get_double() {
    DCHECK(_type == TYPE_DOUBLE);
    return *(reinterpret_cast<double*>(_value));
}

std::string ExtLiteral::get_string() {
    DCHECK(_type == TYPE_VARCHAR || _type == TYPE_CHAR);
    return (reinterpret_cast<StringValue*>(_value))->to_string();
}

std::string ExtLiteral::get_date_string() {
    DCHECK(_type == TYPE_DATE || _type == TYPE_DATETIME);
    DateTimeValue date_value = *reinterpret_cast<DateTimeValue*>(_value);
    if (_type == TYPE_DATE) {
        date_value.cast_to_date();
    }

    char str[MAX_DTVALUE_STR_LEN];
    date_value.to_string(str);
    return std::string(str, strlen(str)); 
}

bool ExtLiteral::get_bool() {
    DCHECK(_type == TYPE_BOOLEAN);
    return *(reinterpret_cast<bool*>(_value));
}

std::string ExtLiteral::get_decimal_string() {
    DCHECK(_type == TYPE_DECIMAL);
    return reinterpret_cast<DecimalValue*>(_value)->to_string();
}

std::string ExtLiteral::get_decimalv2_string() {
    DCHECK(_type == TYPE_DECIMALV2);
    return reinterpret_cast<DecimalV2Value*>(_value)->to_string();
}

std::string ExtLiteral::get_largeint_string() {
    DCHECK(_type == TYPE_LARGEINT);
    return LargeIntValue::to_string(*reinterpret_cast<__int128*>(_value));
}

EsPredicate::EsPredicate(ExprContext* context,
            const TupleDescriptor* tuple_desc) :
    _context(context),
    _disjuncts_num(0),
    _tuple_desc(tuple_desc),
    _es_query_status(Status::OK) {
}

EsPredicate::~EsPredicate() {
    for(int i=0; i < _disjuncts.size(); i++) {
        delete _disjuncts[i];
    }
    _disjuncts.clear();
}

Status EsPredicate::build_disjuncts_list() {
    return build_disjuncts_list(_context->root());
}

// make sure to build by build_disjuncts_list
const vector<ExtPredicate*>& EsPredicate::get_predicate_list(){
    return _disjuncts;
}

static bool ignore_cast(const SlotDescriptor* slot, const Expr* expr) {
    if (slot->type().is_date_type() && expr->type().is_date_type()) {
        return true;
    }
    if (slot->type().is_string_type() && expr->type().is_string_type()) {
        return true;
    }
    return false;
}

static bool is_literal_node(const Expr* expr) {
    switch (expr->node_type()) {
        case TExprNodeType::BOOL_LITERAL:
        case TExprNodeType::INT_LITERAL:
        case TExprNodeType::LARGE_INT_LITERAL:
        case TExprNodeType::FLOAT_LITERAL:
        case TExprNodeType::DECIMAL_LITERAL:
        case TExprNodeType::STRING_LITERAL:
        case TExprNodeType::DATE_LITERAL:
            return true;
        default:
            return false;
    }
}

Status EsPredicate::build_disjuncts_list(const Expr* conjunct) {
    if (TExprNodeType::BINARY_PRED == conjunct->node_type()) {
        if (conjunct->children().size() != 2) {
            return Status("build disjuncts failed: number of childs is not 2");
        }

        SlotRef* slotRef = nullptr;
        TExprOpcode::type op;
        Expr* expr = nullptr;
        if (TExprNodeType::SLOT_REF == conjunct->get_child(0)->node_type()) {
            expr = conjunct->get_child(1);
            slotRef = (SlotRef*)(conjunct->get_child(0));
            op = conjunct->op();
        } else if (TExprNodeType::SLOT_REF == conjunct->get_child(1)->node_type()) {
            expr = conjunct->get_child(0);
            slotRef = (SlotRef*)(conjunct->get_child(1));
            op = conjunct->op();
        } else {
            return Status("build disjuncts failed: no SLOT_REF child");
        }

        const SlotDescriptor* slot_desc = get_slot_desc(slotRef);
        if (slot_desc == nullptr) {
            return Status("build disjuncts failed: slot_desc is null");
        }

        if (!is_literal_node(expr)) {
            return Status("build disjuncts failed: expr is not literal type");
        }

        ExtLiteral literal(expr->type().type, _context->get_value(expr, NULL));
        ExtPredicate* predicate = new ExtBinaryPredicate(
                    TExprNodeType::BINARY_PRED,
                    slot_desc->col_name(),
                    slot_desc->type(),
                    op,
                    literal);

        _disjuncts.push_back(predicate);
        return Status::OK;
    }
    
    if (is_match_func(conjunct)) {
        Expr* expr = conjunct->get_child(1);
        ExtLiteral literal(expr->type().type, _context->get_value(expr, NULL));
        vector<ExtLiteral> query_conditions;
        query_conditions.emplace_back(literal);
        vector<ExtColumnDesc> cols; //TODO
        ExtPredicate* predicate = new ExtFunction(
                        TExprNodeType::FUNCTION_CALL,
                        conjunct->fn().name.function_name,
                        cols,
                        query_conditions);
        if (_es_query_status.ok()) {
            _es_query_status 
                = BooleanQueryBuilder::check_es_query(*(ExtFunction *)predicate); 
            if (!_es_query_status.ok()) {
                return _es_query_status;
            }
        }
        _disjuncts.push_back(predicate);

        return Status::OK;
    } 

    if (TExprNodeType::FUNCTION_CALL == conjunct->node_type()) {
        std::string fname = conjunct->fn().name.function_name;
        if (fname != "like") {
            return Status("build disjuncts failed: function name is not like");
        }

        SlotRef* slotRef = nullptr;
        Expr* expr = nullptr;
        if (TExprNodeType::SLOT_REF == conjunct->get_child(0)->node_type()) {
            expr = conjunct->get_child(1);
            slotRef = (SlotRef*)(conjunct->get_child(0));
        } else if (TExprNodeType::SLOT_REF == conjunct->get_child(1)->node_type()) {
            expr = conjunct->get_child(0);
            slotRef = (SlotRef*)(conjunct->get_child(1));
        } else {
            return Status("build disjuncts failed: no SLOT_REF child");
        }

        const SlotDescriptor* slot_desc = get_slot_desc(slotRef);
        if (slot_desc == nullptr) {
            return Status("build disjuncts failed: slot_desc is null");
        }

        PrimitiveType type = expr->type().type;
        if (type != TYPE_VARCHAR && type != TYPE_CHAR) {
            return Status("build disjuncts failed: like value is not a string");
        }

        ExtLiteral literal(type, _context->get_value(expr, NULL));
        ExtPredicate* predicate = new ExtLikePredicate(
                    TExprNodeType::LIKE_PRED,
                    slot_desc->col_name(),
                    slot_desc->type(),
                    literal);

        _disjuncts.push_back(predicate);
        return Status::OK;
    }
      
    if (TExprNodeType::IN_PRED == conjunct->node_type()) {
        // the op code maybe FILTER_NEW_IN, it means there is function in list
        // like col_a in (abs(1))
        if (TExprOpcode::FILTER_IN != conjunct->op() 
            && TExprOpcode::FILTER_NOT_IN != conjunct->op()) {
            return Status("build disjuncts failed: "
                        "opcode in IN_PRED is neither FILTER_IN nor FILTER_NOT_IN");
        }

        vector<ExtLiteral> in_pred_values;
        const InPredicate* pred = dynamic_cast<const InPredicate*>(conjunct);
        if (Expr::type_without_cast(pred->get_child(0)) != TExprNodeType::SLOT_REF) {
            return Status("build disjuncts failed");
        }

        SlotRef* slot_ref = (SlotRef*)(conjunct->get_child(0));
        const SlotDescriptor* slot_desc = get_slot_desc(slot_ref);
        if (slot_desc == nullptr) {
            return Status("build disjuncts failed: slot_desc is null");
        }

        if (pred->get_child(0)->type().type != slot_desc->type().type) {
            if (!ignore_cast(slot_desc, pred->get_child(0))) {
                return Status("build disjuncts failed");
            }
        }

        HybirdSetBase::IteratorBase* iter = pred->hybird_set()->begin();
        while (iter->has_next()) {
            if (nullptr == iter->get_value()) {
                return Status("build disjuncts failed: hybird set has a null value");
            }

            ExtLiteral literal(slot_desc->type().type, const_cast<void *>(iter->get_value()));
            in_pred_values.emplace_back(literal);
            iter->next();
        }

        ExtPredicate* predicate = new ExtInPredicate(
                    TExprNodeType::IN_PRED,
                    pred->is_not_in(),
                    slot_desc->col_name(),
                    slot_desc->type(),
                    in_pred_values);
        _disjuncts.push_back(predicate);

        return Status::OK;
    } 
    
    if (TExprNodeType::COMPOUND_PRED == conjunct->node_type()) {
        if (TExprOpcode::COMPOUND_OR != conjunct->op()) {
            return Status("build disjuncts failed: op is not COMPOUND_OR");
        }
        Status status = build_disjuncts_list(conjunct->get_child(0));
        if (!status.ok()) {
            return status;
        }
        status = build_disjuncts_list(conjunct->get_child(1));
        if (!status.ok()) {
            return status;
        }

        return Status::OK;
    }

    // if go to here, report error
    std::stringstream ss;
    ss << "build disjuncts failed: node type " << conjunct->node_type() << " is not supported";
    return Status(ss.str());
}

bool EsPredicate::is_match_func(const Expr* conjunct) {
    if (TExprNodeType::FUNCTION_CALL == conjunct->node_type()
        && conjunct->fn().name.function_name == "esquery") {
            return true;
    }
    return false;
}

const SlotDescriptor* EsPredicate::get_slot_desc(const SlotRef* slotRef) {
    const SlotDescriptor* slot_desc = nullptr;
    for (SlotDescriptor* slot : _tuple_desc->slots()) {
        if (slot->id() == slotRef->slot_id()) {
            slot_desc = slot;
            break;
        }
    }
    return slot_desc;
}

}
