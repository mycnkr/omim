package com.mapswithme.yopme.util;

import java.nio.IntBuffer;
import java.util.Arrays;
import java.util.Comparator;

import android.graphics.Bitmap;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.util.Log;

public class PixelBuffer
{
	private final String TAG = "PixelBuffer";
	private EGLDisplay mDisplay;
	private EGLContext mContext;
	private EGLSurface mSurface;
	
	private int mWidth;
	private int mHeight;
	
	public PixelBuffer(int width, int height)
	{
		mWidth = width;
		mHeight = height;
	}
	
	public void init() 
	{
    mDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
    if (mDisplay == EGL14.EGL_NO_DISPLAY)
      throw new EglOperationException("EGL error : No display", EGL14.eglGetError());
		
    int[] versionMajor = new int[4];
    versionMajor[0] = 1;
    int[] versionMinor = new int[4];
    versionMinor[0] = 4;
    if (!EGL14.eglInitialize(mDisplay, versionMajor, 0, versionMinor, 0))
      throw new EglOperationException("EGL error : initialization has failed", EGL14.eglGetError());
		
		EGLConfig[] configs = getConfigs();
		for (int i = 0; i < configs.length; ++i)
		{
			mSurface = createSurface(configs[i]);
      if (mSurface == EGL14.EGL_NO_SURFACE)
				continue;
			
			mContext = createContext(configs[i]);
      if (mContext == EGL14.EGL_NO_CONTEXT)
			{
        EGL14.eglDestroySurface(mDisplay, mSurface);
				continue;
			}
			
			break;
		}
		
    if (mSurface == EGL14.EGL_NO_SURFACE)
      throw new EglOperationException("EGL error : Surface was not created", EGL14.eglGetError());
		
    if (mContext == EGL14.EGL_NO_CONTEXT)
      throw new EglOperationException("EGL error : Context was not created", EGL14.eglGetError());
		
		Log.d(TAG, "Egl inited");
	}
	
	public void terminate()
	{
    if (mDisplay != EGL14.EGL_NO_DISPLAY)
		{
      EGL14.eglMakeCurrent(mDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
      if (mContext != EGL14.EGL_NO_CONTEXT)
        EGL14.eglDestroyContext(mDisplay, mContext);
      if (mSurface != EGL14.EGL_NO_SURFACE)
        EGL14.eglDestroySurface(mDisplay, mSurface);
			
      EGL14.eglTerminate(mDisplay);
		}
		
    mDisplay = EGL14.EGL_NO_DISPLAY;
    mSurface = EGL14.EGL_NO_SURFACE;
    mContext = EGL14.EGL_NO_CONTEXT;
		Log.d(TAG, "Egl terminated");
	}
	
	public void attachToThread()
	{
		Log.d(TAG, "Pixel buffer attached");
    if (!EGL14.eglMakeCurrent(mDisplay, mSurface, mSurface, mContext))
    {
      Log.d(TAG, "Throwed");
      throw new EglOperationException("EGL error : Context was not binded to thread", EGL14.eglGetError());
    }
	}
	
	public void detachFromThread()
	{
		Log.d(TAG, "Pixel buffer detached");
    if (!EGL14.eglMakeCurrent(mDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT))
      throw new EglOperationException("EGL error : Context was not binded to the thread", EGL14.eglGetError());
	}
	
	public Bitmap readBitmap()
	{
    GLES20.glPixelStorei(GLES20.GL_PACK_ALIGNMENT, 1);
    GLES20.glPixelStorei(GLES20.GL_UNPACK_ALIGNMENT, 1);
		
    Log.d(TAG, "Read bitmap. Width = " + mWidth + ", Heigth = " + mHeight);
    IntBuffer ib = IntBuffer.allocate(mWidth * mHeight);
    GLES20.glReadPixels(0, 0, mWidth, mHeight, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, ib);
    
    // Convert upside down mirror-reversed image to right-side up normal image.
    IntBuffer ibt = IntBuffer.allocate(ib.capacity());
    for (int i = 0; i < mHeight; ++i)
      for (int j = 0; j < mWidth; ++j)
        ibt.put((mHeight-i-1)*mWidth + j, ib.get(i*mWidth + j));

    ib = null;
    Bitmap bmp = Bitmap.createBitmap(mWidth, mHeight, Bitmap.Config.ARGB_8888);
    bmp.copyPixelsFromBuffer(ibt);
    return bmp;
	}
	
	class ConfigSorter implements Comparator<EGLConfig>
	{
		@Override
		public int compare(EGLConfig lhs, EGLConfig rhs)
		{
			return getWeight(lhs) - getWeight(rhs);
		}
		
		private int getWeight(EGLConfig config)
		{
			int[] value = new int[1];
      EGL14.eglGetConfigAttrib(mDisplay, config, EGL14.EGL_CONFIG_CAVEAT, value, 0);
			
			switch (value[0]) 
			{
        case EGL14.EGL_NONE:
          return 0;
        case EGL14.EGL_SLOW_CONFIG:
          return 1;
        case EGL14.EGL_NON_CONFORMANT_CONFIG:
          return 2;
			}
			
			return 0;
		}
	}
	
	private EGLContext createContext(EGLConfig config)
	{
    int[] contextAttributes = new int[] { EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE };
    return EGL14.eglCreateContext(mDisplay, config, EGL14.EGL_NO_CONTEXT, contextAttributes, 0);
	}
	
	private EGLSurface createSurface(EGLConfig config)
	{
    int[] surfaceAttribs = new int[] { EGL14.EGL_WIDTH, mWidth, EGL14.EGL_HEIGHT, mHeight, EGL14.EGL_NONE };
    return EGL14.eglCreatePbufferSurface(mDisplay, config, surfaceAttribs, 0);
	}
	
	private EGLConfig[] getConfigs()
	{
		EGLConfig[] configs = new EGLConfig[40];
		int[] numConfigs = new int[] { 0 };
    int[] configAttributes = { EGL14.EGL_RED_SIZE, 5, EGL14.EGL_GREEN_SIZE, 6, EGL14.EGL_BLUE_SIZE, 5,
        EGL14.EGL_STENCIL_SIZE, 0, EGL14.EGL_DEPTH_SIZE, 16, EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
        EGL14.EGL_NONE };
		
    if (!EGL14.eglChooseConfig(mDisplay, configAttributes, 0, configs, 0, configs.length, numConfigs, 0))
      throw new EglOperationException("EGL error : ChooseConfig has failed", EGL14.eglGetError());
		
		if (numConfigs[0] == 0)
      throw new EglOperationException("EGL error : No approproate config found", EGL14.eglGetError());
		
    Log.d(TAG, "Found " + numConfigs[0] + " EGL configs");
		
		EGLConfig[] result = Arrays.copyOf(configs, numConfigs[0]);
		Arrays.sort(result, new ConfigSorter());
		
		return result;		
	}
}
