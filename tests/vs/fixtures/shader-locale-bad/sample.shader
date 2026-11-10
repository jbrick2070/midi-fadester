// A shader whose texts en-US.ini gets wrong in each way check-shader-locale.cmake catches
uniform float alpha <
	string label = "Alpha [Amount]";
	string tooltip = "Moves alpha; zero stops it.";
	float minimum = 0.0;
	float maximum = 1.0;
> = 0.5;

uniform int mode <
	string label = "Mode";
	string widget_type = "select";
	string option_0 = "First";
	string option_1 = "Second";
> = 0;

float4 mainImage(VertData v_in) : TARGET
{
	return image.Sample(textureSampler, v_in.uv) * alpha;
}
