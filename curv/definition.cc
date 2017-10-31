// Copyright Doug Moen 2016-2017.
// Distributed under The MIT License.
// See accompanying file LICENCE.md or https://opensource.org/licenses/MIT

#include <functional>
#include <iostream>
#include <algorithm>

#include <curv/definition.h>
#include <curv/exception.h>
#include <curv/context.h>

namespace curv
{

void
Data_Definition::add_to_scope(Scope& scope)
{
    unsigned unitnum = scope.begin_unit(share(*this));
    slot_ = scope.add_binding(name_, unitnum);
    scope.end_unit(unitnum, share(*this));
}
void
Function_Definition::add_to_scope(Scope& scope)
{
    unsigned unitnum = scope.begin_unit(share(*this));
    slot_ = scope.add_binding(name_, unitnum);
    scope.end_unit(unitnum, share(*this));
}
void
Data_Definition::analyze(Environ& env)
{
    definiens_expr_ = analyze_op(*definiens_phrase_, env);
}
void
Function_Definition::analyze(Environ& env)
{
    lambda_phrase_->shared_nonlocals_ = true;
    auto expr = analyze_op(*lambda_phrase_, env);
    auto lambda = cast<Lambda_Expr>(expr);
    assert(lambda != nullptr);
    lambda_ = make<Lambda>(lambda->body_, lambda->nargs_, lambda->nslots_);
}
Shared<Operation>
Data_Definition::make_setter(slot_t module_slot)
{
    if (module_slot != (slot_t)(-1)) {
        return make<Module_Data_Setter>(source_, module_slot, slot_, definiens_expr_);
    } else {
        return make<Data_Setter>(source_, slot_, definiens_expr_, false);
    }
}
Shared<Operation>
Function_Definition::make_setter(slot_t module_slot)
{
    assert(0);
    return nullptr;
}
void
Compound_Definition_Base::add_to_scope(Scope& scope)
{
    for (auto &e : *this) {
        if (e.definition_ == nullptr)
            scope.add_action(e.phrase_);
        else
            e.definition_->add_to_scope(scope);
    }
}

void
Sequential_Scope::analyze(Abstract_Definition& def)
{
    assert(def.kind_ == Abstract_Definition::k_sequential);
    def.add_to_scope(*this);
    parent_->frame_maxslots_ = frame_maxslots_;
    if (target_is_module_)
        executable_.module_dictionary_ = dictionary_;
}
Shared<Meaning>
Sequential_Scope::single_lookup(const Identifier& id)
{
    auto b = dictionary_->find(id.atom_);
    if (b != dictionary_->end()) {
        if (target_is_module_) {
            return make<Indirect_Strict_Ref>(
                share(id), executable_.module_slot_, b->second);
        } else {
            return make<Let_Ref>(share(id), b->second);
        }
    }
    return nullptr;
}
void
Sequential_Scope::add_action(Shared<const Phrase> phrase)
{
    executable_.actions_.push_back(analyze_action(*phrase, *this));
}
unsigned
Sequential_Scope::begin_unit(Shared<Unitary_Definition> unit)
{
    unit->analyze(*this);
    return 0;
}
slot_t
Sequential_Scope::add_binding(Shared<const Identifier> name, unsigned unitno)
{
    (void)unitno;
    if (dictionary_->find(name->atom_) != dictionary_->end())
        throw Exception(At_Phrase(*name, *parent_),
            stringify(name->atom_, ": multiply defined"));
    slot_t slot = (target_is_module_ ? dictionary_->size() : make_slot());
    (*dictionary_)[name->atom_] = slot;
    return slot;
}
void
Sequential_Scope::end_unit(unsigned unitno, Shared<Unitary_Definition> unit)
{
    (void)unitno;
    executable_.actions_.push_back(
        unit->make_setter(executable_.module_slot_));
}

void
Recursive_Scope::analyze(Abstract_Definition& def)
{
    assert(def.kind_ == Abstract_Definition::k_recursive);
    source_ = def.source_;
    def.add_to_scope(*this);
    for (auto a : action_phrases_) {
        auto op = analyze_op(*a, *this);
        executable_.actions_.push_back(op);
    }
    for (auto& unit : units_) {
        if (unit.state_ == Unit::k_not_analyzed)
            analyze_unit(unit, nullptr);
    }
    parent_->frame_maxslots_ = frame_maxslots_;
    if (target_is_module_) {
        auto d = make<Module::Dictionary>();
        for (auto b : dictionary_)
            (*d)[b.first] = b.second.slot_index_;
        executable_.module_dictionary_ = d;
    }
}

// Analyze the unitary definition `unit` that belongs to the scope,
// then output an action that initializes its bindings to `executable_`.
// As a side effect of analyzing `unit`, all of the units it depends on will
// first be analyzed, and their initialization actions will first be output.
// This ordering means that slots are initialized in dependency order.
//
// Use Tarjan's algorithm for Strongly Connected Components (SCC)
// to group mutually recursive function definitions together into a single
// initialization action.
void
Recursive_Scope::analyze_unit(Unit& unit, const Identifier* id)
{
    switch (unit.state_) {
    case Unit::k_not_analyzed:
        unit.state_ = Unit::k_analysis_in_progress;
        unit.scc_ord_ = unit.scc_lowlink_ = scc_count_++;
        scc_stack_.push_back(&unit);

        analysis_stack_.push_back(&unit);
        if (unit.is_data()) {
            unit.def_->analyze(*this);
        } else {
            Function_Environ fenv(*this, unit);
            unit.def_->analyze(fenv);
            frame_maxslots_ = std::max(frame_maxslots_, fenv.frame_maxslots_);
        }
        analysis_stack_.pop_back();

        if (!analysis_stack_.empty()) {
            Unit* parent = analysis_stack_.back();
            if (unit.scc_lowlink_ < parent->scc_lowlink_) {
                parent->scc_lowlink_ = unit.scc_lowlink_;
                if (unit.is_data()) {
                    throw Exception(At_Phrase(*id, *this),
                        "illegal recursive reference");
                }
            }
        }
        break;
    case Unit::k_analysis_in_progress:
      {
        // Recursion detected. Unit is already on the SCC and analysis stacks.
        if (unit.is_data()) {
            throw Exception(At_Phrase(*id, *this),
                "illegal recursive reference");
        }
        assert(!analysis_stack_.empty());
        Unit* parent = analysis_stack_.back();
        parent->scc_lowlink_ = std::min(parent->scc_lowlink_, unit.scc_ord_);
        // For example, the analysis stack might contain 0->1->2, and now we
        // are back to 0, ie unit.scc_ord_==0 (recursion detected).
        // In the above statement, we are propagating lowlink=0 to unit 2.
        // In the k_not_analyzed case above, once we pop the analysis stack,
        // we'll further propagate 2's lowlink of 0 to unit 1.
        return;
      }
    case Unit::k_analyzed:
        return;
    }
    if (unit.scc_lowlink_ == unit.scc_ord_ /*&& unit.state_ != Unit::k_analyzed*/) {
        // `unit` is the lowest unit in its SCC. All members of this SCC
        // are on the SCC stack. Output an initialization action for unit's SCC.
        if (unit.is_data()) {
            assert(scc_stack_.back() == &unit);
            scc_stack_.pop_back();
            unit.state_ = Unit::k_analyzed;
            executable_.actions_.push_back(
                unit.def_->make_setter(executable_.module_slot_));
        } else {
            // Output a Function_Setter to initialize the slots for a group of
            // mutually recursive functions, or a single nonrecursive function.

            size_t ui = 0;
            while (ui < scc_stack_.size() && scc_stack_[ui] != &unit)
                ++ui;
            assert(scc_stack_[ui] == &unit);

            executable_.actions_.push_back(
                make_function_setter(scc_stack_.size()-ui, &scc_stack_[ui]));
            Unit* u;
            do {
                assert(scc_stack_.size() > 0);
                u = scc_stack_.back();
                scc_stack_.pop_back();
                assert(u->scc_ord_ == unit.scc_ord_);
                u->state_ = Unit::k_analyzed;
            } while (u != &unit);
        }
    }
}

Shared<Operation>
Recursive_Scope::make_function_setter(size_t nunits, Unit** units)
{
    Shared<const Phrase> source = nunits==1 ? units[0]->def_->source_ : source_;
    Shared<Module::Dictionary> nonlocal_dictionary = make<Module::Dictionary>();
    std::vector<Shared<Operation>> nonlocal_exprs;
    slot_t slot = 0;

    std::vector<Shared<Function_Definition>> funs;
    funs.reserve(nunits);
    for (size_t u = 0; u < nunits; ++u) {
        if (auto f = cast<Function_Definition>(units[u]->def_)) {
            funs.push_back(f);
            (*nonlocal_dictionary)[f->name_->atom_] = slot++;
            nonlocal_exprs.push_back(
                make<Constant>(Shared<const Phrase>{f->lambda_phrase_}, Value{f->lambda_}));
        } else
            throw Exception(At_Phrase(*units[u]->def_->source_, *this),
                "recursive data definition");
    }

    for (size_t u = 0; u < nunits; ++u) {
        for (auto b : units[u]->nonlocals_) {
            if (nonlocal_dictionary->find(b.first) == nonlocal_dictionary->end()) {
                (*nonlocal_dictionary)[b.first] = slot++;
                nonlocal_exprs.push_back(b.second);
            }
        }
    }

    Shared<Enum_Module_Expr> nonlocals = make<Enum_Module_Expr>(
        source, nonlocal_dictionary, nonlocal_exprs);
    Shared<Function_Setter> setter =
        Function_Setter::make(nunits, source, executable_.module_slot_, nonlocals);
    for (size_t i = 0; i < nunits; ++i)
        setter->at(i) = {funs[i]->slot_, funs[i]->lambda_};
    return setter;
}

// How do I report illegal recursion? Eg,
// f->data->f
//   f()=x;
//   x=f();
// Report "illegal recursive reference" for either the x or f reference.
// Specifically, it's a recursive reference in a data definition that's bad.
// So "illegal recursive reference" for the f reference.

Shared<Meaning>
Recursive_Scope::single_lookup(const Identifier& id)
{
    auto b = dictionary_.find(id.atom_);
    if (b != dictionary_.end()) {
        analyze_unit(units_[b->second.unit_index_], &id);
        if (target_is_module_) {
            return make<Indirect_Strict_Ref>(
                share(id), executable_.module_slot_, b->second.slot_index_);
        } else {
            return make<Let_Ref>(share(id), b->second.slot_index_);
        }
    }
    return nullptr;
}
Shared<Meaning>
Recursive_Scope::Function_Environ::single_lookup(const Identifier& id)
{
    Shared<Meaning> m = scope_.lookup(id);
    if (isa<Constant>(m))
        return m;
    if (auto expr = cast<Operation>(m)) {
        unit_.nonlocals_[id.atom_] = expr;
        return make<Symbolic_Ref>(share(id));
    }
    return m;
}
void
Recursive_Scope::add_action(Shared<const Phrase> phrase)
{
    action_phrases_.push_back(phrase);
}
unsigned
Recursive_Scope::begin_unit(Shared<Unitary_Definition> def)
{
    units_.emplace_back(def);
    return units_.size() - 1;
}
slot_t
Recursive_Scope::add_binding(Shared<const Identifier> name, unsigned unitno)
{
    if (dictionary_.find(name->atom_) != dictionary_.end())
        throw Exception(At_Phrase(*name, *parent_),
            stringify(name->atom_, ": multiply defined"));
    slot_t slot = (target_is_module_ ? dictionary_.size() : make_slot());
    dictionary_.emplace(
        std::make_pair(name->atom_, Binding{slot, unitno}));
    return slot;
}
void
Recursive_Scope::end_unit(unsigned unitno, Shared<Unitary_Definition> unit)
{
    (void)unitno;
    (void)unit;
}

} // namespace curv
