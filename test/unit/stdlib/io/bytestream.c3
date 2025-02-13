module std::io @test;

fn void! bytestream()
{
	ByteReader r;
	r.init("abc");
	Stream* s = &r;
	assert(s.len()! == 3);
	char[5] buffer;
	assert('a' == s.read_byte()!);
	s.pushback_byte()!;
	usz len = s.read(&buffer)!;
	assert((String)buffer[:len] == "abc");
	ByteWriter w;
	w.init();
	Stream* ws = &w;
	ws.write("helloworld")!;
	assert(w.str_view() == "helloworld");
	s.seek(0, SET)!;
	ws.read_from(s)!;
	s.seek(1, SET)!;
	s.write_to(ws)!;
	assert(w.str_view() == "helloworldabcbc");
}

fn void! bytewriter_buffer()
{
	ByteWriter writer;
	char[8] z;
	writer.init_buffer(&z);
	Stream* s = &writer;
	s.write("hello")!!;
	s.write_byte(0)!!;
	String o = ((ZString)&z).str_view();
	assert(o == "hello");
	assert(@catch(s.write("xxxx")));
}

fn void! bytewriter_read_from()
{
    char[] data = "Lorem ipsum dolor sit amet biam.";
    TestReader r = { .bytes = data };
    Stream* s = &&r.as_stream();

    ByteWriter bw;
    bw.tinit();
    bw.read_from(s)!;

    assert(bw.str_view() == data);
}

module std::io;
// TestReader only has the read method to trigger the path
// in ByteWriter.read_from that does not rely on the available method.
struct TestReader
{
    char[] bytes;
    usz index;
}

fn StreamWrapper TestReader.as_stream(TestReader *r)
{
    return { .stream = { &testReader_interface }, .data = r };
}

fn usz! TestReader.read(TestReader *r, char[] bytes)
{
    usz left = r.bytes.len - r.index;
    if (left == 0) return 0;
    usz n = min(left, bytes.len);
    mem::copy(bytes.ptr, &r.bytes[r.index], n);
    r.index += n;
    return n;
}

StreamInterface testReader_interface = {
    .read_fn = fn(s, char[] bytes) => ((TestReader*)((StreamWrapper*)s).data).read(bytes),
};