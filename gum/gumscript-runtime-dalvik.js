/*
 * TODO
 *
 * Dalvik:
 *   - Expose methods
 *   - Hook methods
 *   - Use WeakMap to clean up wrappers when they go out of scope
 *   - Make it possible to implement a Java interface in JavaScript
 *
 * Runtime:
 *   - onUnload
 *   - Process.pointerSize
 *   - NativePointer: isNull()
 *   - Thread.isFrida()
 *   - global NULL constant
 */

(function () {
    var _runtime = null;
    var _api = null;
    var pointerSize = (Process.arch === 'x64' || Process.arch === 'arm64') ? 8 : 4; // TODO: runtime should expose the pointer size
    var scratchBuffer = Memory.alloc(pointerSize);
    var NULL = ptr("0");
    var JNI_OK = 0;
    var JNI_VERSION_1_6 = 0x00010006;

    var CONSTRUCTOR_METHOD = 1;
    var CLASS_METHOD = 2;
    var INSTANCE_METHOD = 3;

    Object.defineProperty(this, 'Dalvik', {
        enumerable: true,
        get: function () {
            if (_runtime === null) {
                _runtime = new Runtime();
            }
            return _runtime;
        }
    });

    var Runtime = function Runtime() {
        var api = null;
        var vm = null;
        var classFactory = null;
        var pendingCallbacks = [];
        var scheduledCallbacks = [];

        var initialize = function () {
            api = getApi();
            if (api !== null) {
                vm = new VM(api);
                classFactory = new ClassFactory(api, vm);
            }
        };

        Object.defineProperty(this, 'available', {
            enumerable: true,
            get: function () {
                return api !== null;
            }
        });

        this.perform = function (fn) {
            if (api === null) {
                throw new Error("Dalvik runtime not available");
            }

            var env = vm.tryGetEnv();
            var alreadyAttached = env !== null;
            if (!alreadyAttached) {
                env = vm.attachCurrentThread();
            }

            var pendingException = null;
            //try {
                fn();
            //} catch (e) {
            //    pendingException = e;
            //}

            if (!alreadyAttached) {
                vm.detachCurrentThread();
            }

            if (pendingException !== null) {
                throw pendingException;
            }
        };

        this.use = function (className) {
            return classFactory.use(className);
        };

        this.cast = function (handle, C) {
            return classFactory.cast(handle, C);
        };

        this.implement = function (method, fn) {
            return new NativeCallback(fn, method.returnType, method.argumentTypes);
        };

        initialize.call(this);
    };

    var VM = function VM(api, vm) {
        var handle = null;
        var attachCurrentThread = null;
        var detachCurrentThread = null;
        var getEnv = null;

        var initialize = function () {
            handle = Memory.readPointer(api.gDvmJni.add(8));

            var vtable = Memory.readPointer(handle);
            attachCurrentThread = new NativeFunction(Memory.readPointer(vtable.add(4 * pointerSize)), 'int', ['pointer', 'pointer', 'pointer']);
            detachCurrentThread = new NativeFunction(Memory.readPointer(vtable.add(5 * pointerSize)), 'int', ['pointer']);
            getEnv = new NativeFunction(Memory.readPointer(vtable.add(6 * pointerSize)), 'int', ['pointer', 'pointer', 'int']);
        };

        this.attachCurrentThread = function () {
            checkJniResult("VM::AttachCurrentThread", attachCurrentThread(handle, scratchBuffer, NULL));
            return new Env(Memory.readPointer(scratchBuffer));
        };

        this.detachCurrentThread = function () {
            checkJniResult("VM::DetachCurrentThread", detachCurrentThread(handle));
        };

        this.getEnv = function () {
            checkJniResult("VM::GetEnv", getEnv(handle, scratchBuffer, JNI_VERSION_1_6));
            return new Env(Memory.readPointer(scratchBuffer));
        };

        this.tryGetEnv = function () {
            var result = getEnv(handle, scratchBuffer, JNI_VERSION_1_6);
            if (result !== JNI_OK) {
                return null;
            }
            return new Env(Memory.readPointer(scratchBuffer));
        };

        initialize.call(this);
    };

    function Env(handle) {
        this.handle = handle;
    }

    (function () {
        var cachedVtable = null;

        var CALL_CONSTRUCTOR_METHOD_OFFSET = 28;
        var CALL_OBJECT_METHOD_OFFSET = 34;
        var CALL_VOID_METHOD_OFFSET = 61;

        function vtable() {
            if (cachedVtable === null) {
                cachedVtable = Memory.readPointer(this.handle);
            }
            return cachedVtable;
        }

        function proxy(offset, retType, argTypes, wrapper) {
            var impl = null;
            return function () {
                if (impl === null) {
                    impl = new NativeFunction(Memory.readPointer(vtable.call(this).add(offset * pointerSize)), retType, argTypes);
                }
                var args = [impl];
                args = args.concat.apply(args, arguments);
                return wrapper.apply(this, args);
            };
        }

        Env.prototype.findClass = proxy(6, 'pointer', ['pointer', 'pointer'], function (impl, name) {
            return impl(this.handle, Memory.allocUtf8String(name));
        });

        Env.prototype.fromReflectedMethod = proxy(7, 'pointer', ['pointer', 'pointer'], function (impl, method) {
            return impl(this.handle, method);
        });

        Env.prototype.getSuperclass = proxy(10, 'pointer', ['pointer', 'pointer'], function (impl, klass) {
            return impl(this.handle, klass);
        });

        Env.prototype.exceptionOccurred = proxy(15, 'pointer', ['pointer'], function (impl) {
            return impl(this.handle);
        });

        Env.prototype.exceptionDescribe = proxy(16, 'void', ['pointer'], function (impl) {
            impl(this.handle);
        });

        Env.prototype.exceptionClear = proxy(17, 'void', ['pointer'], function (impl) {
            impl(this.handle);
        });

        Env.prototype.pushLocalFrame = proxy(19, 'int', ['pointer', 'int'], function (impl, capacity) {
            return impl(this.handle, capacity);
        });

        Env.prototype.popLocalFrame = proxy(20, 'pointer', ['pointer', 'pointer'], function (impl, result) {
            return impl(this.handle, result);
        });

        Env.prototype.newGlobalRef = proxy(21, 'pointer', ['pointer', 'pointer'], function (impl, obj) {
            return impl(this.handle, obj);
        });

        Env.prototype.deleteGlobalRef = proxy(22, 'void', ['pointer', 'pointer'], function (impl, globalRef) {
            impl(this.handle, globalRef);
        });

        Env.prototype.deleteLocalRef = proxy(23, 'void', ['pointer', 'pointer'], function (impl, localRef) {
            impl(this.handle, localRef);
        });

        Env.prototype.isSameObject = proxy(24, 'uint8', ['pointer', 'pointer', 'pointer'], function (impl, ref1, ref2) {
            return impl(this.handle, ref1, ref2) ? true : false;
        });

        Env.prototype.getMethodId = proxy(33, 'pointer', ['pointer', 'pointer', 'pointer', 'pointer'], function (impl, klass, name, sig) {
            return impl(this.handle, klass, Memory.allocUtf8String(name), Memory.allocUtf8String(sig));
        });

        Env.prototype.getStaticMethodID = proxy(113, 'pointer', ['pointer', 'pointer', 'pointer', 'pointer'], function (impl, klass, name, sig) {
            return impl(this.handle, klass, Memory.allocUtf8String(name), Memory.allocUtf8String(sig));
        });

        Env.prototype.newStringUtf = proxy(167, 'pointer', ['pointer', 'pointer'], function (impl, str) {
            var utf = Memory.allocUtf8String(str);
            return impl(this.handle, utf);
        });

        Env.prototype.getStringUtfChars = proxy(169, 'pointer', ['pointer', 'pointer', 'pointer'], function (impl, str) {
            return impl(this.handle, str, NULL);
        });

        Env.prototype.releaseStringUtfChars = proxy(170, 'void', ['pointer', 'pointer', 'pointer'], function (impl, str, utf) {
            impl(this.handle, str, utf);
        });

        Env.prototype.getArrayLength = proxy(171, 'int', ['pointer', 'pointer'], function (impl, array) {
            return impl(this.handle, array);
        });

        Env.prototype.getObjectArrayElement = proxy(173, 'pointer', ['pointer', 'pointer', 'int'], function (impl, array, index) {
            return impl(this.handle, array, index);
        });

        var cachedMethods = {};
        var method = function (offset, retType, argTypes) {
            var key = offset + "|" + retType + "|" + argTypes.join(":");
            var m = cachedMethods[key];
            if (!m) {
                m = new NativeFunction(Memory.readPointer(vtable.call(this).add(offset * pointerSize)), retType, ['pointer', 'pointer', 'pointer', '...'].concat(argTypes));
                cachedMethods[key] = m;
            }
            return m;
        };

        Env.prototype.constructor = function (argTypes) {
            return method(CALL_CONSTRUCTOR_METHOD_OFFSET, 'pointer', argTypes);
        };

        Env.prototype.method = function (retType, argTypes) {
            var offset;
            if (retType === 'pointer') {
                offset = CALL_OBJECT_METHOD_OFFSET;
            } else if (retType === 'void') {
                offset = CALL_VOID_METHOD_OFFSET;
            } else {
                throw new Error("Unsupported type: " + retType + " (pull-request welcome!)");
            }
            return method(offset, retType, argTypes);
        };

        var javaLangClass = null;
        Env.prototype.javaLangClass = function () {
            if (javaLangClass === null) {
                var handle = this.findClass("java/lang/Class");
                javaLangClass = {
                    getName: this.getMethodId(handle, "getName", "()Ljava/lang/String;"),
                    getDeclaredConstructors: this.getMethodId(handle, "getDeclaredConstructors", "()[Ljava/lang/reflect/Constructor;"),
                    getDeclaredMethods: this.getMethodId(handle, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;")
                };
                this.deleteLocalRef(handle);
            }
            return javaLangClass;
        };

        var javaLangObject = null;
        Env.prototype.javaLangObject = function () {
            if (javaLangObject === null) {
                var handle = this.findClass("java/lang/Object");
                javaLangObject = {
                    toString: this.getMethodId(handle, "toString", "()Ljava/lang/String;")
                };
                this.deleteLocalRef(handle);
            }
            return javaLangObject;
        };

        var javaLangReflectConstructor = null;
        Env.prototype.javaLangReflectConstructor = function () {
            if (javaLangReflectConstructor === null) {
                var handle = this.findClass("java/lang/reflect/Constructor");
                javaLangReflectConstructor = {
                    handle: this.newGlobalRef(handle),
                    getGenericParameterTypes: this.getMethodId(handle, "getGenericParameterTypes", "()[Ljava/lang/reflect/Type;")
                };
                this.deleteLocalRef(handle);
            }
            return javaLangReflectConstructor;
        };

        Env.prototype.getClassName = function (klass) {
            var name = this.method('pointer', [])(this.handle, klass, this.javaLangClass().getName);
            var result = this.stringFromJni(name);
            this.deleteLocalRef(name);
            return result;
        };

        Env.prototype.stringFromJni = function (str) {
            var utf = this.getStringUtfChars(str);
            var result = Memory.readUtf8String(utf);
            this.releaseStringUtfChars(str, utf);
            return result;
        };
    })();

    var ClassFactory = function ClassFactory(api, vm) {
        var factory = this;
        var classes = {};

        var initialize = function () {
            api = getApi();
        };

        this.use = function (className) {
            var klass = classes[className];
            if (!klass) {
                var env = vm.getEnv();
                var handle = env.findClass(className.replace(/\./g, "/"));
                if (handle.toString(16) === "0") {
                    throw new Error("Class '" + className + "' is not loaded");
                }
                var C = ensureClass(handle, className);
                klass = new C(handle, null);
                env.deleteLocalRef(handle);
            }
            return klass;
        };

        this.cast = function (handle, C) {
            return new C(C.__handle__, handle);
        };

        var ensureClass = function (classHandle, cachedName) {
            var env = vm.getEnv();

            var name = cachedName !== null ? cachedName : env.getClassName(classHandle);
            var klass = classes[name];
            if (klass) {
                return klass;
            }

            var superHandle = env.getSuperclass(classHandle);
            var superKlass;
            if (superHandle.toString(16) !== "0") {
                superKlass = ensureClass(superHandle, null);
                env.deleteLocalRef(superHandle);
            } else {
                superKlass = null;
            }

            eval("klass = function " + basename(name) + "(classHandle, handle) {" +
                 "this.$class = klass;" +
                 "this.$classHandle = env.newGlobalRef(classHandle);" +
                 "this.$handle = (handle !== null) ? env.newGlobalRef(handle) : null;" +
            "};");

            classes[name] = klass;

            var initializeClass = function initializeClass() {
                klass.__name__ = name;
                klass.__handle__ = env.newGlobalRef(classHandle);

                klass.prototype.$new = makeConstructor();

                klass.prototype.toString = function toString () {
                    return name;
                };
            };

            var makeConstructor = function () {
                var Constructor = env.javaLangReflectConstructor();
                var invokeObjectMethodNoArgs = env.method('pointer', []);

                var methods = [];
                var retType = objectType(name);
                var constructors = invokeObjectMethodNoArgs(env.handle, classHandle, env.javaLangClass().getDeclaredConstructors);
                var numConstructors = env.getArrayLength(constructors);
                for (var constructorIndex = 0; constructorIndex !== numConstructors; constructorIndex++) {
                    var constructor = env.getObjectArrayElement(constructors, constructorIndex);

                    var methodId = env.fromReflectedMethod(constructor);
                    var methodArgTypes = [];

                    var types = invokeObjectMethodNoArgs(env.handle, constructor, Constructor.getGenericParameterTypes);
                    env.deleteLocalRef(constructor);
                    var numTypes = env.getArrayLength(types);
                    try {
                        for (var typeIndex = 0; typeIndex !== numTypes; typeIndex++) {
                            var t = env.getObjectArrayElement(types, typeIndex);
                            try {
                                var argType = typeFromClassName(env.getClassName(t));
                                methodArgTypes.push(argType);
                            } finally {
                                env.deleteLocalRef(t);
                            }
                        }
                    } catch (e) {
                        continue;
                    } finally {
                        env.deleteLocalRef(types);
                    }

                    methods.push(makeMethod(CONSTRUCTOR_METHOD, methodId, retType, methodArgTypes));
                }
                env.deleteLocalRef(constructors);

                return makeMethodDispatcher(methods);
            };

            var makeMethodDispatcher = function (methods) {
                var candidates = {};
                methods.forEach(function (m) {
                    var numArgs = m.argumentTypes.length;
                    var group = candidates[numArgs];
                    if (!group) {
                        group = [];
                        candidates[numArgs] = group;
                    }
                    group.push(m);
                });

                return function () {
                    var group = candidates[arguments.length];
                    if (!group) {
                        throw new Error("Argument count does not match any overload");
                    }
                    for (var i = 0; i !== group.length; i++) {
                        var method = group[i];
                        if (method.canInvokeWith(arguments)) {
                            return method.apply(this, arguments);
                        }
                    }
                    throw new Error("Argument types do not match any overload");
                };
            };

            var makeMethod = function (type, methodId, retType, argTypes) {
                var rawRetType = retType.type;
                var rawArgTypes = argTypes.map(function (t) { return t.type; });
                var invokeTarget = (type == CONSTRUCTOR_METHOD) ? env.constructor(rawArgTypes) : env.method(rawRetType, rawArgTypes);

                var frameCapacity = 2;
                var argVariableNames = argTypes.map(function (t, i) {
                    return "a" + (i + 1);
                });
                var callArgs = [
                    "env.handle",
                    type === INSTANCE_METHOD ? "this.$handle" : "this.$classHandle",
                    "methodId"
                ].concat(argTypes.map(function (t, i) {
                    if (t.toJni) {
                        frameCapacity++;
                        return "argTypes[" + i + "].toJni.call(this, " + argVariableNames[i] + ", env)";
                    }
                    return argVariableNames[i];
                }));
                var returnCapture, returnStatement;
                if (rawRetType === 'void') {
                    returnCapture = "";
                    returnStatements = "env.popLocalFrame(NULL);";
                } else {
                    if (retType.fromJni) {
                        frameCapacity++;
                        returnCapture = "var rawResult = ";
                        returnStatements = "var result = retType.fromJni.call(this, rawResult, env);" +
                            "env.popLocalFrame(NULL);" +
                            "return result;";
                    } else {
                        returnCapture = "var result = ";
                        returnStatements = "env.popLocalFrame(NULL);" +
                            "return result;";
                    }
                }
                eval("var f = function (" + argVariableNames.join(", ") + ") {" +
                    "var env = vm.getEnv();" +
                    "if (env.pushLocalFrame(" + frameCapacity + ") !== JNI_OK) {" +
                        "env.exceptionClear();" +
                        "throw new Error(\"Out of memory\");" +
                    "}" +
                    returnCapture + "invokeTarget(" + callArgs.join(", ") + ");" +
                    "var throwable = env.exceptionOccurred();" +
                    "if (throwable.toString(16) !== \"0\") {" +
                        "env.exceptionClear();" +
                        "var description = env.method('pointer', [])(env.handle, throwable, env.javaLangObject().toString);" +
                        "var descriptionStr = env.stringFromJni(description);" +
                        "env.popLocalFrame(NULL);" +
                        "throw new Error(descriptionStr);" +
                    "}" +
                    returnStatements +
                "}");

                Object.defineProperty(f, 'returnType', {
                    enumerable: true,
                    value: rawRetType
                });

                Object.defineProperty(f, 'argumentTypes', {
                    enumerable: true,
                    value: rawArgTypes
                });

                f.canInvokeWith = function (args) {
                    if (args.length !== argTypes.length) {
                        return false;
                    }

                    return argTypes.every(function (t, i) {
                        return t.isCompatible(args[i]);
                    });
                };

                return f;
            };

            if (superKlass !== null) {
                var Surrogate = function () {
                    this.constructor = klass;
                };
                Surrogate.prototype = superKlass.prototype;
                klass.prototype = new Surrogate();

                klass.__super__ = superKlass.prototype;
            } else {
                klass.__super__ = null;
            }

            initializeClass();

            return klass;
        };

        var typeFromClassName = function (className) {
            var type = types[className];
            if (!type && className.indexOf("[") === 0) {
                throw new Error("Unsupported type: " + className);
            }
            return objectType(className);
        };

        var types = {
            'boolean': {
                type: 'uint8',
                isCompatible: function (v) {
                    return typeof v === 'boolean';
                },
                fromJni: function (v) {
                    return v ? true : false;
                },
                toJni: function (v) {
                    return v ? 1 : 0;
                }
            },
            'byte': {
                type: 'int8',
                isCompatible: function (v) {
                    return typeof v === 'number';
                }
            },
            'char': {
                type: 'uint16',
                isCompatible: function (v) {
                    return typeof v === 'string' && v.length === 1;
                }
            },
            'short': {
                type: 'int16',
                isCompatible: function (v) {
                    return typeof v === 'number';
                }
            },
            'int': {
                type: 'int32',
                isCompatible: function (v) {
                    return typeof v === 'number';
                }
            },
            'long': {
                type: 'int64',
                isCompatible: function (v) {
                    return typeof v === 'number';
                }
            },
            'float': {
                type: 'float',
                isCompatible: function (v) {
                    return typeof v === 'number';
                }
            },
            'double': {
                type: 'double',
                isCompatible: function (v) {
                    return typeof v === 'number';
                }
            },
            '[B': {
                type: 'pointer',
                isCompatible: function (v) {
                    return typeof v === 'object' && v.hasOwnProperty('length');
                },
                fromJni: function () {
                    throw new Error("Not yet implemented ([B)");
                },
                toJni: function () {
                    throw new Error("Not yet implemented ([B)");
                }
            },
            '[C': {
                type: 'pointer',
                isCompatible: function (v) {
                    return typeof v === 'object' && v.hasOwnProperty('length');
                },
                fromJni: function () {
                    throw new Error("Not yet implemented ([C)");
                },
                toJni: function () {
                    throw new Error("Not yet implemented ([C)");
                }
            },
            '[I': {
                type: 'pointer',
                isCompatible: function (v) {
                    return typeof v === 'object' && v.hasOwnProperty('length');
                },
                fromJni: function () {
                    throw new Error("Not yet implemented ([I)");
                },
                toJni: function () {
                    throw new Error("Not yet implemented ([I)");
                }
            },
            'java.lang.String': {
                type: 'pointer',
                isCompatible: function (v) {
                    return typeof v === 'string';
                },
                fromJni: function (h, env) {
                    return env.stringFromJni(h);
                },
                toJni: function (s, env) {
                    return env.newStringUtf(h);
                }
            }
        };

        var objectType = function (className) {
            return {
                type: 'pointer',
                isCompatible: function (v) {
                    if (className === 'java.lang.String' && typeof v === 'string') {
                        return true;
                    }

                    return typeof v === 'object' && v.hasOwnProperty('$handle'); // TODO: improve strictness
                },
                fromJni: function (h, env) {
                    if (h.toString(16) === "0") {
                        return null;
                    } else if (this.$handle !== null && env.isSameObject(h, this.$handle)) {
                        return this;
                    } else {
                        return factory.cast(h, factory.use(className));
                    }
                },
                toJni: function (o, env) {
                    if (typeof o === 'string') {
                        return env.newStringUtf(o);
                    }

                    return o.$handle;
                }
            };
        };

        initialize.call(this);
    };

    var checkJniResult = function (name, result) {
        if (result != JNI_OK) {
            throw new Error(name + " failed: " + result);
        }
    };

    var basename = function (className) {
        return className.slice(className.lastIndexOf(".") + 1);
    };

    var getApi = function () {
        if (_api !== null) {
            return _api;
        }

        var temporaryApi = {};
        var pending = [
            {
                module: "libdvm.so",
                functions: {
                    "_Z18dvmFindLoadedClassPKc": ["dvmFindLoadedClass", 'pointer', ['pointer']]
                },
                variables: {
                    "gDvmJni": function (address) {
                        this.gDvmJni = address;
                    }
                }
            }
        ];
        var remaining = 0;
        pending.forEach(function (api) {
            var pendingFunctions = api.functions;
            var pendingVariables = api.variables;
            remaining += Object.keys(pendingFunctions).length + Object.keys(pendingVariables).length;
            Module.enumerateExports(api.module, {
                onMatch: function (exp) {
                    var name = exp.name;
                    if (exp.type === 'function') {
                        var signature = pendingFunctions[name];
                        if (signature) {
                            if (typeof signature === 'function') {
                                signature.call(temporaryApi, exp.address);
                            } else {
                                temporaryApi[signature[0]] = new NativeFunction(exp.address, signature[1], signature[2]);
                            }
                            delete pendingFunctions[name];
                            remaining--;
                        }
                    } else if (exp.type === 'variable') {
                        var handler = pendingVariables[name];
                        if (handler) {
                            handler.call(temporaryApi, exp.address);
                            delete pendingVariables[name];
                            remaining--;
                        }
                    }
                    if (remaining === 0) {
                        return 'stop';
                    }
                },
                onComplete: function () {
                }
            });
        });
        if (remaining === 0) {
            _api = temporaryApi;
        }

        return _api;
    };
}).call(this);

send("Dalvik.available: " + Dalvik.available);
Dalvik.perform(function () {
    var javaLangString = Dalvik.use("java.lang.String");
    var s = javaLangString.$new("Hello Java!");
    send(s);
});
