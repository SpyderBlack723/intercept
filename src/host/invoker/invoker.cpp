﻿#include "invoker.hpp"
#include "controller.hpp"
#include "extensions.hpp"
#include "shared/client_types.hpp"
#ifdef __linux__
#include <dlfcn.h>
#include <link.h>
#endif

namespace intercept {
    //game_data_string_pool<> invoker::string_pool;

    /*!
    @brief Flag for when the invoker is able to access the RV Engine
    */

    bool invoker::invoker_accessible = false;
    bool invoker::invoker_accessible_all = false;

    unary_function invoker::_register_hook_trampoline = nullptr;
    uintptr_t invoker::sqf_game_state = 0;

    invoker::invoker() : _thread_count(0), _patched(false), _attached(false) {

    }

    invoker::~invoker() {

    }

    void invoker::attach_controller() {
        if (!_attached) {
            _attached = true;
            controller::get().add("init_invoker", std::bind(&intercept::invoker::init_invoker, this, std::placeholders::_1, std::placeholders::_2));
            controller::get().add("test_invoker", std::bind(&intercept::invoker::test_invoker, this, std::placeholders::_1, std::placeholders::_2));
            controller::get().add("do_invoke_period", [](const arguments & args_, std::string & result_) {
                return false; //#deprecate
            });
            controller::get().add("invoker_begin_register", std::bind(&intercept::invoker::invoker_begin_register, this, std::placeholders::_1, std::placeholders::_2));
            controller::get().add("invoker_register", std::bind(&intercept::invoker::invoker_register, this, std::placeholders::_1, std::placeholders::_2));
            controller::get().add("invoker_end_register", std::bind(&intercept::invoker::invoker_end_register, this, std::placeholders::_1, std::placeholders::_2));
            controller::get().add("rv_event", [](const arguments & args_, std::string & result_) {
                return false; //#deprecate
            });
            controller::get().add("signal", [](const arguments & args_, std::string & result_) {
                return false; //#deprecate
            });
            eventhandlers::get().initialize();
        }
    }

    bool invoker::invoker_begin_register(const arguments & args_, std::string & result_) {

        sqf_functions::get().initialize();
        _intercept_registerTypes_function = sqf_functions::get().registerFunction("interceptRegisterTypes", "", userFunctionWrapper_ref<_intercept_registerTypes>, types::GameDataType::BOOL, types::GameDataType::NAMESPACE);
        _intercept_event_function = sqf_functions::get().registerFunction("interceptEvent", "", userFunctionWrapper<_intercept_event>, types::GameDataType::BOOL, types::GameDataType::STRING, types::GameDataType::ARRAY);
        _intercept_do_invoke_period_function = sqf_functions::get().registerFunction("interceptOnFrame", "", userFunctionWrapper<_intercept_do_invoke_period>, types::GameDataType::BOOL);
        _intercept_signal_function = sqf_functions::get().registerFunction("interceptSignal", "", userFunctionWrapper<_intercept_signal>, types::GameDataType::BOOL, types::GameDataType::ARRAY, types::GameDataType::ARRAY);
        //#deprecate
        //if (loader::get().hook_function("str", _register_hook, _register_hook_trampoline)) {
        //    LOG(INFO) << "Registration function hooked.";
        //}
        //else {
        //    LOG(ERROR) << "Registration function failed to hook.";
        //}
        return true;
    }

    bool invoker::invoker_register(const arguments & args_, std::string & result_) {
        _registration_type = args_.as_string(0);
        return true;
    }

    bool invoker::invoker_end_register(const arguments & args_, std::string & result_) {
        //#TODO move pre_start to here
        _intercept_registerTypes_function.clear(); //#TEST
        sqf_functions::get().setDisabled();
        //#deprecate
        //if (loader::get().unhook_function("str", _register_hook, _register_hook_trampoline)) {
        //    LOG(INFO) << "Registration function unhooked.";
        //}
        //else {
        //    LOG(INFO) << "Registration function failed to unhook.";
        //}

        return true;
    }

    game_value invoker::_intercept_do_invoke_period() {
        return invoker::get().do_invoke_period();
    }

    bool invoker::do_invoke_period() {
        {
            _invoker_unlock period_lock(this, true);
            long timeout = clock() + 3;
            while (_thread_count > 0 && clock() < timeout) std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
        {
            _invoker_unlock on_frame_lock(this);
            // do the per-frame handler here.
            for (auto& module : extensions::get().modules()) {
                if (module.second.functions.on_frame) {
                    module.second.functions.on_frame();
                }
            }
        }
        return true;
    }

    game_value invoker::_intercept_event(game_value left_arg_, game_value right_arg_) {
        return invoker::get().rv_event(left_arg_, right_arg_);
    }

    bool invoker::rv_event(const std::string& event_name_, game_value& params_) {
        LOG(DEBUG) << "EH " << event_name_ << " START";
        auto handler = _eventhandlers.find(event_name_);
        if (handler != _eventhandlers.end()) {
            bool all = false;
            // If we are stopping a mission it is assumed that threads will be
            // stopped and joined here. Deadlocks can occur if we do not open up
            // the invoker to all threads.
            if (event_name_ == "mission_stopped")
                all = true;
            _invoker_unlock eh_lock(this, all);
            //game_value params = invoke_raw_nolock(_get_variable_func, &_mission_namespace, &var_name);
            handler->second(params_);
            LOG(DEBUG) << "EH " << event_name_ << " END";
            return true;
        }
        return false;
    }

    game_value invoker::_intercept_signal(game_value left_arg_, game_value right_arg_) {
        return invoker::get().signal(left_arg_[0], left_arg_[1], right_arg_);
    }

    bool invoker::signal(const std::string& extension_name, const std::string& signal_name, game_value args) {
        LOG(DEBUG) << "Signal " << extension_name << " : " << signal_name << " START";

        auto signal_module = extensions::get().modules().find(extension_name);
        if (signal_module == extensions::get().modules().end()) {
            return false;
        }
        auto signal_func_it = signal_module->second.signal_funcs.find(signal_name);
        module::on_signal_func signal_func;
        if (signal_func_it == signal_module->second.signal_funcs.end()) {
        #ifdef __linux__
            signal_func = reinterpret_cast<module::on_signal_func>(dlsym(signal_module->second.handle, signal_name.c_str()));
        #else
            signal_func = reinterpret_cast<module::on_signal_func>(GetProcAddress(signal_module->second.handle, signal_name.c_str())); //#TODO why?! The signal module function thingy is commented out.. also has a #TODO with ?! on it
        #endif
            if (!signal_func)
                return false;
            else
                signal_module->second.signal_funcs[signal_name] = signal_func;
        } else {
            signal_func = signal_func_it->second;
        }
        _invoker_unlock signal_lock(this);
        signal_func(args);
        LOG(DEBUG) << "Signal " << extension_name << " : " << signal_name << " END";
        return true;
    }

    bool invoker::init_invoker(const arguments & args_, std::string & result_) {
        //_eh_params_name = "intercept_params_var";
        _invoker_unlock init_lock(this);
        //_mission_namespace = invoke_raw("missionnamespace");
        //loader::get().get_function("getvariable", _get_variable_func, "NAMESPACE", "STRING");
        //_eh_params = invoke_raw_nolock(_get_variable_func, _mission_namespace, _eh_params_name);
        //game_value p_name = "intercept_signal_var";
        //_signal_params = invoke_raw_nolock(_get_variable_func, _mission_namespace, p_name);
        return true;
    }

    bool invoker::test_invoker(const arguments & args_, std::string & result_) {
        _invoker_unlock test_lock(this);
        result_ = "-1";
        game_value res = invoke_raw("profilenamesteam");
        result_ = static_cast<std::string>(res);
        return true;
    }

    game_value invoker::invoke_raw_nolock(nular_function function_) {
        //;  //#TODO change nular_function definition to take game_value*
    #ifdef __linux
        return function_(invoker::sqf_game_state);
    #else
        game_value ret;
        function_(&ret, invoker::sqf_game_state); return ret;
    #endif
    }

    game_value invoker::invoke_raw(const std::string &function_name_) const {
        nular_function function;
        if (loader::get().get_function(function_name_, function)) {
            return invoke_raw_nolock(function);
        }
        return game_value();
    }

    game_value invoker::invoke_raw_nolock(unary_function function_, const game_value &right_arg_) {
    #ifdef __linux
        return function_(invoker::sqf_game_state, reinterpret_cast<uintptr_t>(&right_arg_));
    #else
        game_value ret;
        function_(&ret, invoker::sqf_game_state, reinterpret_cast<uintptr_t>(&right_arg_)); return ret;
    #endif
    }

    game_value invoker::invoke_raw(const std::string &function_name_, const game_value &right_, const std::string &right_type_) const {
        unary_function function;
        if (loader::get().get_function(function_name_, function, right_type_)) {
            return invoke_raw_nolock(function, right_);
        }
        return game_value();
    }

    game_value invoker::invoke_raw(const std::string &function_name_, const game_value &right_) const {
        unary_function function;
        if (loader::get().get_function(function_name_, function)) {
            return invoke_raw_nolock(function, right_);
        }
        return game_value();
    }


    game_value invoker::invoke_raw_nolock(binary_function function_, const game_value &left_arg_, const game_value &right_arg_) {
        auto left = reinterpret_cast<uintptr_t>(&left_arg_);
        auto right = reinterpret_cast<uintptr_t>(&right_arg_);
    #ifdef __linux
        return function_(invoker::sqf_game_state, left, right);
    #else
        game_value ret;
        function_(&ret, invoker::sqf_game_state, left, right); return ret;
    #endif
    }

    game_value invoker::invoke_raw(const std::string &function_name_, const game_value &left_, const game_value &right_) const {
        binary_function function;
        if (loader::get().get_function(function_name_, function)) {
            return invoke_raw_nolock(function, left_, right_);
        }
        return game_value();
    }

    game_value invoker::invoke_raw(const std::string &function_name_, const game_value &left_, const std::string &left_type_, const game_value &right_, const std::string &right_type_) const {
        binary_function function;
        if (loader::get().get_function(function_name_, function, left_type_, right_type_)) {
            return invoke_raw_nolock(function, left_, right_);
        }
        return game_value();
    }

    value_type invoker::get_type(const game_value &value_) {
        return value_.type();
    }

    const std::string& invoker::get_type_str(const game_value &value_) const {
        auto type = value_.type();
        return type_map.at(type);
    }

    game_value invoker::_intercept_registerTypes(const game_value& left_arg_) {
        using GameDataType = types::GameDataType;

        auto regInfo = loader::get().get_register_sqf_info();

        LOG(INFO) << "Registration Hook Function Called: " << invoker::get()._registration_type;
        auto step = invoker::get()._registration_type;
        invoker::get()._sqf_game_state = regInfo._gameState;
        sqf_game_state = regInfo._gameState;

        game_value::__vptr_def = left_arg_.get_vtable();
        invoker::get().type_structures["GV"] = { game_value::__vptr_def ,game_value::__vptr_def };

        ref<game_data> gd_ar(regInfo._types[static_cast<size_t>(GameDataType::ARRAY)]->_createFunction(nullptr));
        std::pair<value_type, value_type> structure = { gd_ar->get_vtable(), gd_ar->get_secondary_vtable() };
        invoker::get().type_map[structure.first] = "ARRAY";
        invoker::get().type_structures["ARRAY"] = structure;
        game_data_array::type_def = structure.first;
        game_data_array::data_type_def = structure.second;
        game_data_array::pool_alloc_base = loader::get().get_allocator()->_poolAllocs[static_cast<size_t>(types::GameDataType::ARRAY)];
        ref<game_data> gd_sc(regInfo._types[static_cast<size_t>(GameDataType::SCALAR)]->_createFunction(nullptr));
        structure = { gd_sc->get_vtable(), gd_sc->get_secondary_vtable() };
        invoker::get().type_map[structure.first] = "SCALAR";
        invoker::get().type_structures["SCALAR"] = structure;
        game_data_number::type_def = structure.first;
        game_data_number::data_type_def = structure.second;
        game_data_number::pool_alloc_base = loader::get().get_allocator()->_poolAllocs[static_cast<size_t>(types::GameDataType::SCALAR)];
        ref<game_data> gd_bo(regInfo._types[static_cast<size_t>(GameDataType::BOOL)]->_createFunction(nullptr));
        structure = { gd_bo->get_vtable(), gd_bo->get_secondary_vtable() };
        invoker::get().type_map[structure.first] = "BOOL";
        invoker::get().type_structures["BOOL"] = structure;
        game_data_bool::type_def = structure.first;
        game_data_bool::data_type_def = structure.second;
        game_data_bool::pool_alloc_base = loader::get().get_allocator()->_poolAllocs[static_cast<size_t>(types::GameDataType::BOOL)];
        ref<game_data> gd_st(regInfo._types[static_cast<size_t>(GameDataType::STRING)]->_createFunction(nullptr));
        structure = { gd_st->get_vtable(), gd_st->get_secondary_vtable() };
        invoker::get().type_map[structure.first] = "STRING";
        invoker::get().type_structures["STRING"] = structure;
        game_data_string::type_def = structure.first;
        game_data_string::data_type_def = structure.second;
        game_data_string::pool_alloc_base = loader::get().get_allocator()->_poolAllocs[static_cast<size_t>(types::GameDataType::STRING)];
        ref<game_data> gd_code(regInfo._types[static_cast<size_t>(GameDataType::CODE)]->_createFunction(nullptr));
        structure = { gd_code->get_vtable(), gd_code->get_secondary_vtable() };
        invoker::get().type_map[structure.first] = "CODE";
        invoker::get().type_structures["CODE"] = structure;
        game_data_code::type_def = structure.first;
        game_data_code::data_type_def = structure.second;
        ref<game_data> gd_ob(regInfo._types[static_cast<size_t>(GameDataType::OBJECT)]->_createFunction(nullptr));
        structure = { gd_ob->get_vtable(), gd_ob->get_secondary_vtable() };
        invoker::get().type_map[structure.first] = "OBJECT";
        invoker::get().type_structures["OBJECT"] = structure;
        game_data_object::type_def = structure.first;
        game_data_object::data_type_def = structure.second;
        ref<game_data> gd_gr(regInfo._types[static_cast<size_t>(GameDataType::GROUP)]->_createFunction(nullptr));
        structure = { gd_gr->get_vtable(), gd_gr->get_secondary_vtable() };
        invoker::get().type_map[structure.first] = "GROUP";
        invoker::get().type_structures["GROUP"] = structure;
        game_data_group::type_def = structure.first;
        game_data_group::data_type_def = structure.second;
        ref<game_data> gd_conf(regInfo._types[static_cast<size_t>(GameDataType::CONFIG)]->_createFunction(nullptr));
        structure = { gd_conf->get_vtable(), gd_conf->get_secondary_vtable() };
        invoker::get().type_map[structure.first] = "CONFIG";
        invoker::get().type_structures["CONFIG"] = structure;
        game_data_config::type_def = structure.first;
        game_data_config::data_type_def = structure.second;
        ref<game_data> gd_cont(regInfo._types[static_cast<size_t>(GameDataType::CONTROL)]->_createFunction(nullptr));
        structure = { gd_cont->get_vtable(), gd_cont->get_secondary_vtable() };
        invoker::get().type_map[structure.first] = "CONTROL";
        invoker::get().type_structures["CONTROL"] = structure;
        game_data_control::type_def = structure.first;
        game_data_control::data_type_def = structure.second;
        ref<game_data> gd_di(regInfo._types[static_cast<size_t>(GameDataType::DISPLAY)]->_createFunction(nullptr));
        structure = { gd_di->get_vtable(), gd_di->get_secondary_vtable() };
        invoker::get().type_map[structure.first] = "DISPLAY";
        invoker::get().type_structures["DISPLAY"] = structure;
        game_data_display::type_def = structure.first;
        game_data_display::data_type_def = structure.second;
        ref<game_data> gd_loc(regInfo._types[static_cast<size_t>(GameDataType::LOCATION)]->_createFunction(nullptr));
        structure = { gd_loc->get_vtable(), gd_loc->get_secondary_vtable() };
        invoker::get().type_map[structure.first] = "LOCATION";
        invoker::get().type_structures["LOCATION"] = structure;
        game_data_location::type_def = structure.first;
        game_data_location::data_type_def = structure.second;
        ref<game_data> gd_scr(regInfo._types[static_cast<size_t>(GameDataType::SCRIPT)]->_createFunction(nullptr));
        structure = { gd_scr->get_vtable(), gd_scr->get_secondary_vtable() };
        invoker::get().type_map[structure.first] = "SCRIPT";
        invoker::get().type_structures["SCRIPT"] = structure;
        game_data_script::type_def = structure.first;
        game_data_script::data_type_def = structure.second;
        ref<game_data> gd_si(regInfo._types[static_cast<size_t>(GameDataType::SIDE)]->_createFunction(nullptr));
        structure = { gd_si->get_vtable(), gd_si->get_secondary_vtable() };
        invoker::get().type_map[structure.first] = "SIDE";
        invoker::get().type_structures["SIDE"] = structure;
        game_data_side::type_def = structure.first;
        game_data_side::data_type_def = structure.second;
        ref<game_data> gd_te(regInfo._types[static_cast<size_t>(GameDataType::TEXT)]->_createFunction(nullptr));
        structure = { gd_te->get_vtable(), gd_te->get_secondary_vtable() };
        invoker::get().type_map[structure.first] = "TEXT";
        invoker::get().type_structures["TEXT"] = structure;
        game_data_rv_text::type_def = structure.first;
        game_data_rv_text::data_type_def = structure.second;
        ref<game_data> gd_tm(regInfo._types[static_cast<size_t>(GameDataType::TEAM_MEMBER)]->_createFunction(nullptr));
        structure = { gd_tm->get_vtable(), gd_tm->get_secondary_vtable() };
        invoker::get().type_map[structure.first] = "TEAM_MEMBER";
        invoker::get().type_structures["TEAM_MEMBER"] = structure;
        game_data_team::type_def = structure.first;
        game_data_team::data_type_def = structure.second;


        structure = { left_arg_.data->get_vtable(), left_arg_.data->get_secondary_vtable() };
        invoker::get().type_map[structure.first] = "NAMESPACE";
        invoker::get().type_structures["NAMESPACE"] = structure;
        game_data_rv_namespace::type_def = structure.first;
        game_data_rv_namespace::data_type_def = structure.second;
        LOG(INFO) << "invoker::_intercept_registerTypes done\n";
        return true;
    }

    bool invoker::add_eventhandler(const std::string & name_, std::function<void(game_value&)> func_) {
        if (_eventhandlers.find(name_) != _eventhandlers.end()) {
            // @TODO: Exceptions
            return false;
        }
        _eventhandlers[name_] = func_;

        return true;
    }

    void invoker::lock() {
        _thread_count = _thread_count + 1;
        std::unique_lock<std::mutex> lock(_state_mutex);
        _invoke_condition.wait(lock, [] {return invoker_accessible_all; });
        _invoke_mutex.lock();

        LOG(DEBUG) << "Client Thread ACQUIRE EXCLUSIVE";
    }

    void invoker::unlock() {
        _invoke_mutex.unlock();
        LOG(DEBUG) << "Client Thread RELEASE EXCLUSIVE";
        _thread_count = _thread_count - 1;
    }

    invoker::_invoker_unlock::_invoker_unlock(invoker * instance_, bool all_threads_, bool delayed_) : _unlocked(false), _instance(instance_), _all(all_threads_) {
        if (!delayed_) {
            unlock();
        }
    }

    invoker::_invoker_unlock::~_invoker_unlock() {
        if (_unlocked) {
            std::lock_guard<std::mutex> lock(_instance->_state_mutex);
            if (_all) {
                std::lock_guard<std::recursive_mutex> invoke_lock(_instance->_invoke_mutex);
                invoker_accessible = false;
                invoker_accessible_all = false;
                LOG(DEBUG) << "LOCKED ALL";
            } else {
                invoker_accessible = false;
                LOG(DEBUG) << "LOCKED";
            }

        }
    }

    void invoker::_invoker_unlock::unlock() {
        if (!_unlocked) {
            if (_all) {
                LOG(DEBUG) << "UNLOCKING ALL";
                std::unique_lock<std::recursive_mutex> invoke_lock(_instance->_invoke_mutex, std::defer_lock);
                {
                    std::lock_guard<std::mutex> lock(_instance->_state_mutex);
                    invoke_lock.lock();
                    invoker_accessible = true;
                    invoker_accessible_all = true;
                }
                _instance->_invoke_condition.notify_all();
            } else {
                LOG(DEBUG) << "UNLOCKING";
                std::lock_guard<std::mutex> lock(_instance->_state_mutex);
                invoker_accessible = true;
            }
            _unlocked = true;
        }
    }
}
