/*
 * A34: camtl=1 -- CoreAnimation's Metal renderer in backboardd. HackiOS installs these from its
 * backboardd injection (userland/src/inject.c, specialize_without_apple; MIT); here the plug-in
 * installs them itself, when Metal loads it in backboardd and boot-arg camtl=1 is given, so
 * nothing changes in the OpenGL ES path (userland/a34gl) and nothing is installed otherwise.
 *
 * Function constants: CoreAnimation specializes QuartzCore's uber shaders with constant values
 * (-newFunctionWithDescriptor:error:, -newFunctionWithName:constantValues:...), which Metal hands
 * to Apple's compiler service; that service compiles for Apple's GPUs, not for this one. So
 * nothing is specialized by Apple: the plain function is answered, with the values CoreAnimation
 * set kept on it by Metal's index (associated object "redwoodFunctionConstants", which
 * rw_function_constants() above hands to the compiler library: rwa_render_constants).
 * HackiOS turned a uchar3 into a uchar4 because their library was macOS 26's; ours is this
 * QuartzCore's own, so values go through as they were set.
 */
typedef struct { unsigned long location, length; } RWNSRange;

static void (*rw_orig_set_constant_index)(id, SEL, const void *, unsigned long, unsigned long);
static void (*rw_orig_set_constant_name)(id, SEL, const void *, unsigned long, id);
static void (*rw_orig_set_constant_range)(id, SEL, const void *, unsigned long, RWNSRange);

static size_t
rw_constant_size(unsigned long type)
{
	static const struct { unsigned long first, elem; } groups[] = {
		{ 3, 4 } /* Float */, { 16, 2 } /* Half */, { 29, 4 } /* Int */, { 33, 4 } /* UInt */,
		{ 37, 2 } /* Short */, { 41, 2 } /* UShort */, { 45, 1 } /* Char */, { 49, 1 } /* UChar */,
		{ 53, 1 } /* Bool */,
	};

	for (unsigned i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
		if (type >= groups[i].first && type < groups[i].first + 4) {
			return groups[i].elem * (type - groups[i].first + 1);
		}
	}
	return type == 81 || type == 85 ? 8 : 0;        /* Long, ULong */
}

static void
rw_keep_constant(id values, id key, const void *value, unsigned long type)
{
	SEL slot = sel_registerName("redwoodConstantValues");
	NSMutableDictionary *kept = objc_getAssociatedObject(values, slot);
	uint64_t bits = 0;
	size_t size = rw_constant_size(type);

	if (value == NULL || size == 0) {
		return;
	}
	memcpy(&bits, value, size < 8 ? size : 8);
	if (kept == nil) {
		kept = [[NSMutableDictionary alloc] init];
		objc_setAssociatedObject(values, slot, kept, OBJC_ASSOCIATION_RETAIN);
		[kept release];
	}
	[kept setObject:[NSNumber numberWithUnsignedLongLong:bits] forKey:key];
}

static void
rw_set_constant_index(id values, SEL selector, const void *value, unsigned long type, unsigned long index)
{
	rw_keep_constant(values, [NSNumber numberWithUnsignedLong:index], value, type);
	rw_orig_set_constant_index(values, selector, value, type, index);
}

static void
rw_set_constant_name(id values, SEL selector, const void *value, unsigned long type, id name)
{
	rw_keep_constant(values, name, value, type);
	rw_orig_set_constant_name(values, selector, value, type, name);
}

static void
rw_set_constant_range(id values, SEL selector, const void *value, unsigned long type, RWNSRange range)
{
	size_t size = rw_constant_size(type);

	for (unsigned long i = 0; value != NULL && size != 0 && i < range.length; i++) {
		rw_keep_constant(values, [NSNumber numberWithUnsignedLong:range.location + i],
		    (const uint8_t *)value + i * size, type);
	}
	rw_orig_set_constant_range(values, selector, value, type, range);
}

/* the plain function, with the values CoreAnimation set kept on it by Metal's index */
static id
rw_plain_function(id library, id name, id values)
{
	id function = [library newFunctionWithName:name];
	NSDictionary *kept = values ? objc_getAssociatedObject(values, sel_registerName("redwoodConstantValues")) : nil;
	NSMutableDictionary *by_index;
	NSDictionary *constants;

	if (function == nil || kept == nil) {
		return function;
	}
	by_index = [[NSMutableDictionary alloc] init];
	constants = [function respondsToSelector:sel_registerName("functionConstantsDictionary")] ?
	    [function performSelector:sel_registerName("functionConstantsDictionary")] : nil;
	for (id key in kept) {
		id value = [kept objectForKey:key];

		if ([key isKindOfClass:[NSString class]]) {
			id constant = [constants objectForKey:key];

			if (constant == nil || ![constant respondsToSelector:sel_registerName("index")]) {
				continue;
			}
			key = [NSNumber numberWithUnsignedLong:((NSUInteger (*)(id, SEL))objc_msgSend)(constant,
			    sel_registerName("index"))];
		}
		[by_index setObject:value forKey:key];
	}
	objc_setAssociatedObject(function, sel_registerName("redwoodFunctionConstants"), by_index, OBJC_ASSOCIATION_RETAIN);
	[by_index release];
	return function;
}

static id
rw_new_function_values_error(id library, SEL selector, id name, id values, NSError **error)
{
	(void)selector;
	id function = rw_plain_function(library, name, values);

	if (function == nil && error != NULL) {
		*error = nil;
	}
	return function;
}

static void
rw_new_function_values_handler(id library, SEL selector, id name, id values, void (^handler)(id, NSError *))
{
	(void)selector;
	id function = rw_plain_function(library, name, values);

	handler(function, nil);
	[function release];
}

static struct { Class cls; IMP imp; } rw_descriptor_originals[16];
static unsigned rw_descriptor_original_count;

/* -newFunctionWithDescriptor:error:, what QuartzCore 16 sends: the descriptor carries the name and
 * the constant values. Every class that implements it is hooked, its own implementation kept. */
static id
rw_new_function_descriptor(id library, SEL selector, id descriptor, NSError **error)
{
	id name = [descriptor performSelector:sel_registerName("name")];
	id values = [descriptor performSelector:sel_registerName("constantValues")];

	if (name != nil && values != nil) {
		id function = rw_plain_function(library, name, values);

		if (function != nil || error == NULL) {
			return function;
		}
	}
	for (Class cls = object_getClass(library); cls != Nil; cls = class_getSuperclass(cls)) {
		for (unsigned i = 0; i < rw_descriptor_original_count; i++) {
			if (rw_descriptor_originals[i].cls == cls) {
				return ((id (*)(id, SEL, id, NSError **))rw_descriptor_originals[i].imp)(library, selector,
				    descriptor, error);
			}
		}
	}
	return nil;
}

static int
rw_hook_function_descriptors(void)
{
	SEL selector = sel_registerName("newFunctionWithDescriptor:error:");
	Protocol *protocol = objc_getProtocol("MTLLibrary");
	unsigned count = 0;
	Class *classes = objc_copyClassList(&count);
	int hooked = 0;

	for (unsigned i = 0; classes != NULL && i < count && rw_descriptor_original_count < 16; i++) {
		unsigned n = 0;
		Method *methods;

		if (protocol != NULL && !class_conformsToProtocol(classes[i], protocol)) {
			continue;
		}
		methods = class_copyMethodList(classes[i], &n);
		for (unsigned k = 0; k < n && rw_descriptor_original_count < 16; k++) {
			if (method_getName(methods[k]) == selector) {
				rw_descriptor_originals[rw_descriptor_original_count].cls = classes[i];
				rw_descriptor_originals[rw_descriptor_original_count++].imp =
				    method_setImplementation(methods[k], (IMP)rw_new_function_descriptor);
				rw_print("camtl=1: -[%s newFunctionWithDescriptor:error:] specializes without Apple\n",
				    class_getName(classes[i]));
				hooked++;
			}
		}
		free(methods);
	}
	free(classes);
	return hooked;
}

/* once, in backboardd with camtl=1 (or wherever A34METAL_SPECIALIZE=1 asks for it) */
static void
rw_camtl_install(void)
{
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		Class values = objc_getClass("MTLFunctionConstantValuesInternal");
		Class library = objc_getClass("_MTLLibrary");
		Method m;
		int done = 0;

		if (values == Nil) {
			values = objc_getClass("MTLFunctionConstantValues");
		}
		if ((m = class_getInstanceMethod(values, sel_registerName("setConstantValue:type:atIndex:"))) != NULL) {
			rw_orig_set_constant_index = (void (*)(id, SEL, const void *, unsigned long, unsigned long))
			    method_setImplementation(m, (IMP)rw_set_constant_index);
			done++;
		}
		if ((m = class_getInstanceMethod(values, sel_registerName("setConstantValue:type:withName:"))) != NULL) {
			rw_orig_set_constant_name = (void (*)(id, SEL, const void *, unsigned long, id))
			    method_setImplementation(m, (IMP)rw_set_constant_name);
			done++;
		}
		if ((m = class_getInstanceMethod(values, sel_registerName("setConstantValues:type:withRange:"))) != NULL) {
			rw_orig_set_constant_range = (void (*)(id, SEL, const void *, unsigned long, RWNSRange))
			    method_setImplementation(m, (IMP)rw_set_constant_range);
			done++;
		}
		if (library != Nil &&
		    (m = class_getInstanceMethod(library, sel_registerName("newFunctionWithName:constantValues:error:"))) != NULL) {
			method_setImplementation(m, (IMP)rw_new_function_values_error);
			done++;
		}
		if (library != Nil && (m = class_getInstanceMethod(library,
		    sel_registerName("newFunctionWithName:constantValues:completionHandler:"))) != NULL) {
			method_setImplementation(m, (IMP)rw_new_function_values_handler);
			done++;
		}
		done += rw_hook_function_descriptors();
		rw_print("camtl=1: function constants are kept for the compiler library, not specialized by Apple "
		    "(%d hooks)\n", done);
	});
}
